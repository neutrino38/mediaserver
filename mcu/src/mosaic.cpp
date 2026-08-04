#include "log.h"
#include "mosaic.h"
#include "partedmosaic.h"
#include "asymmetricmosaic.h"
#include "pipmosaic.h"
#include <vector>
#include <map>
#include <string.h>
#include <stdlib.h>

// Mémorise la trame d'un slot pour la composition par graphe avfilter (Phase 3).
// N'écrit plus un pixel dans le buffer BYTE* : le composite est produit par
// MosaicCompositor (cf. GetPict). Les trames GPU (VAAPI) sont conservées TELLES
// QUELLES depuis la Phase 5 : c'est le graphe qui décide (scale_vaapi en mode
// GPU, hwdownload intégré à la branche en mode CPU) — plus aucune redescente
// hors graphe.
int Mosaic::Update(int index, const PictPtr& pic)
{
	if (!pic || !pic->GetAVFrame())
		return 0;

	// Mémorise la trame et l'aspect du slot (lus par BuildDesc/GetPict).
	if (index >= 0 && index < (int) slotFrames.size())
	{
		slotFrames[index]     = pic;
		slotKeepAspect[index] = keepAspect;
	}

	SetChanged();
	return 1;
}

// Calcule la taille effective de la vignette (letterbox/pillarbox) et son décalage
// dans le slot. Reprend l'arithmétique des Update(BYTE*) historiques
// (partedmosaic.cpp / asymmetricmosaic.cpp), diff vertical rendu pair (offset U/V),
// à une correction près : la comparaison se fait contre le ratio du SLOT et non
// contre le ratio GLOBAL de la mosaïque (membre 'ratio'). Le code historique était
// juste par coïncidence — toutes les dispositions en grille ont des cellules au
// ratio de la toile — mais débordait du slot dès qu'une cellule s'en écartait, ce
// qui interdisait toute disposition non homothétique (cf. mosaic1p1 pleine hauteur).
// Liseré effectif d'un slot : SlotBorder px de chaque côté, sauf slot trop
// petit (aucun type réel n'est concerné, pure défense).
int Mosaic::GetSlotBorder(int pos)
{
	const int slotW = GetWidth(pos);
	const int slotH = GetHeight(pos);
	if (slotW > 4 * SlotBorder && slotH > 4 * SlotBorder)
		return SlotBorder;
	return 0;
}

void Mosaic::ComputeSlotPlacement(int pos, int inW, int inH, bool keepAspect,
                                  int& outW, int& outH, int& dx, int& dy)
{
	// Slot UTILE : le liseré noir est réservé sur les quatre côtés ; l'image
	// (et son letterbox) se placent dedans, le pad du graphe remplit le liseré.
	const int b     = GetSlotBorder(pos);
	const int slotW = GetWidth(pos)  - 2 * b;
	const int slotH = GetHeight(pos) - 2 * b;

	//Par défaut : remplit le slot utile, décalé du liseré.
	outW = slotW;
	outH = slotH;
	dx   = b;
	dy   = b;

	if (inW <= 0 || inH <= 0)
		return;

	// StretchSlot force l'étirement plein slot utile (PIP pos==0).
	if (StretchSlot(pos))
		return;

	DWORD picRatio = ComputeAspectRatio(inW, inH);

	// Ratio du slot lui-même (brut : ce n'est pas une résolution de caméra, donc
	// pas de passage par ComputeAspectRatio et sa normalisation CIF/SIF/VGA).
	if (slotH <= 0)
		return;
	DWORD slotRatio = (DWORD) ((slotW * 1000) / slotH);

	if ((picRatio / 100) == (slotRatio / 100) || !keepAspect)
	{
		// Même ratio (à ~1% près) ou ancien comportement : remplit le slot utile.
		return;
	}
	else if (picRatio > slotRatio)
	{
		// Bandes haut/bas : conserve la largeur, réduit la hauteur.
		outW = slotW;
		outH = (int) ((slotW * 1000) / picRatio);
		int diff = (slotH - outH) / 2;
		// diff pair sinon offset U/V incorrect (cf. code historique).
		if (diff > 0 && (diff % 2) != 0) diff--;
		dx = b;
		dy = b + diff;
	}
	else // picRatio < slotRatio
	{
		// Bandes gauche/droite : conserve la hauteur, réduit la largeur.
		outH = slotH;
		outW = (int) ((slotH * picRatio) / 1000);
		int diff = (slotW - outW) / 2;
		// diff pair comme en vertical : l'origine de la vignette (x-liseré)
		// reste paire, donc l'offset du filtre overlay aussi — un offset impair
		// serait arrondi par le chroma 4:2:0 et décalerait l'image d'un pixel.
		if (diff > 0 && (diff % 2) != 0) diff--;
		dx = b + diff;
		dy = b;
	}
}

// Construit la description du graphe de composition d'après l'état courant des
// slots (trames mémorisées). Slots ACTIFS uniquement (trame non nulle), ordre =
// ordre Z (pos croissant). Matérialise aussi les Pict RGBA des overlays
// (participant par slot + mosaïque plein cadre) dans slotOverlayPicts /
// mosaicOverlayPict — rendu Overlay en cache : un re-rendu même taille (ex.
// changement de nom) change le Pict mais PAS la desc, donc pas de reconstruction
// de graphe, juste une recomposition.
MosaicGraphDesc Mosaic::BuildDesc()
{
	MosaicGraphDesc desc;
	desc.width           = mosaicTotalWidth;
	desc.height          = mosaicTotalHeight;
	desc.wantGPU         = false;   // décidé après la boucle (politique §2.1)
	desc.hasMosaicOverlay = false;
	desc.blackBackground = HasBlackBackground();

	slotOverlayPicts.clear();

	bool anyGPUInput = false;

	for (int pos = 0; pos < numSlots && pos < (int) slotFrames.size(); pos++)
	{
		const PictPtr& pic = slotFrames[pos];
		if (!pic || !pic->GetAVFrame())
			continue;   // slot vide : pas de branche, fond visible

		AVFrame* f = pic->GetAVFrame();

		MosaicSlotDesc s;
		s.pos    = pos;
		s.border = GetSlotBorder(pos);
		s.inW    = f->width;
		s.inH    = f->height;
		s.inFmt  = f->format;
		s.hwFramesCtx = f->hw_frames_ctx;   // non nul ssi trame GPU (clé de reconfig)
		if (pic->IsGPUPict())
			anyGPUInput = true;

		int dx, dy;
		ComputeSlotPlacement(pos, f->width, f->height, slotKeepAspect[pos],
		                     s.w, s.h, dx, dy);
		s.x = GetLeft(pos) + dx;
		s.y = GetTop(pos)  + dy;

		// Overlay participant : dimensionné au slot UTILE (liseré déduit, pour
		// ne jamais recouvrir le liseré ni le slot voisin ; historiquement il
		// couvrait le slot entier). ovW/ovH repris du Pict effectivement rendu
		// (source de vérité du buffersrc) ; un rendu indisponible (nullptr)
		// désactive l'overlay du slot pour ce tick plutôt que de faire échouer
		// Compose.
		PictPtr ovPict;
		const int partId = mosaicPos[pos];
		if (partId > 0)
		{
			auto ito = overlays.find(partId);
			if (ito != overlays.end() && ito->second && ito->second->HasContent())
			{
				ito->second->Resize(GetWidth(pos)  - 2 * s.border,
				                    GetHeight(pos) - 2 * s.border);
				ovPict = ito->second->GetPict();
				if (ovPict && ovPict->GetAVFrame())
				{
					s.hasOverlay = true;
					s.ovX = GetLeft(pos) + s.border;
					s.ovY = GetTop(pos)  + s.border;
					s.ovW = ovPict->GetAVFrame()->width;
					s.ovH = ovPict->GetAVFrame()->height;
				}
				else
					ovPict.reset();   // rendu indisponible : slot sans overlay ce tick
			}
		}

		desc.slots.push_back(s);
		slotOverlayPicts.push_back(ovPict);   // aligné sur desc.slots
	}

	// Overlay mosaïque plein cadre (par-dessus toutes les vignettes).
	mosaicOverlayPict.reset();
	if (overlay && overlay->HasContent())
	{
		overlay->Resize(mosaicTotalWidth, mosaicTotalHeight);
		PictPtr mp = overlay->GetPict();
		if (mp && mp->GetAVFrame())
		{
			desc.hasMosaicOverlay = true;
			mosaicOverlayPict = mp;
		}
	}

	// Politique GPU (§2.1 du plan) : composer sur GPU seulement si le device
	// VAAPI partagé existe ET qu'au moins une entrée est déjà une surface GPU —
	// sans entrée GPU, tout monter en VRAM serait une pure perte (uploads puis
	// probable redescente côté encodeur logiciel). Le compositor peut encore
	// replier en CPU (échec de config, slots superposés type PIP).
	desc.wantGPU = anyGPUInput && Pict::GetVAAPIDevice() != nullptr;

	return desc;
}

// Composite de la mosaïque via le graphe avfilter (MosaicCompositor). Une seule
// composition par tick grâce au cache 'compositeValid' (invalidé par SetChanged
// sur tout Update/Clean) : plusieurs inputs partageant la mosaïque n'entraînent
// qu'une compo. En cas d'échec de (re)configuration on ressert le dernier
// composite connu (éventuellement nul) — le chemin BYTE* de repli a disparu
// en Phase 6.
PictPtr Mosaic::GetPict()
{
	if (compositeValid && composite)
		return composite;

	MosaicGraphDesc desc = BuildDesc();
	if (!compositor.Configure(desc))
	{
		Error("-Mosaic: echec Configure du compositor, composite précédent resservi\n");
		return composite;
	}

	// Fond (généré une fois : taille et couleur fixes pour cette mosaïque).
	const PictPtr& bg = GetBackground();
	if (!bg)
		return composite;

	// Trames des slots ACTIFS, alignées sur desc.slots (même ordre/positions).
	std::vector<PictPtr> frames;
	frames.reserve(desc.slots.size());
	for (const MosaicSlotDesc& s : desc.slots)
		frames.push_back(slotFrames[s.pos]);

	// Overlays matérialisés par BuildDesc (alignés sur desc.slots).
	PictPtr out = compositor.Compose(frames, slotOverlayPicts, bg, mosaicOverlayPict);
	if (out)
	{
		composite      = out;
		compositeValid = true;
	}
	return composite;
}

// Fond de la mosaïque, généré paresseusement (couleur/taille fixes).
const PictPtr& Mosaic::GetBackground()
{
	if (!background)
	{
		if (HasBlackBackground())
			background = Pict::CreateBlack(mosaicTotalWidth, mosaicTotalHeight);
		else
			background = Pict::CreateColor(mosaicTotalWidth, mosaicTotalHeight,
			                               128, 128, 128);
	}
	return background;
}

int Mosaic::GetNumSlotsForType(Mosaic::Type type)
{
	//Depending on the type
	switch(type)
	{
		case mosaic1x1:
			return 1;
		case mosaic2x2:
			return 4;
		case mosaic3x3:
			return 9;
		case mosaic3p4:
			return  7;
		case mosaic1p7:
			return 8;
		case mosaic1p5:
			return  6;
		case mosaic1p1:
			return 2;
		case mosaicPIP1:
			return 2;
		case mosaicPIP3:
			return 4;
		case mosaic4x4:
			return 16;
		case mosaic1p4:
			return 16;
		case mosaic2p8:
			return 10;
	}
	//Error
	return Error("-Unknown mosaic type %d\n",type);
}

Mosaic::Mosaic(Type type,DWORD size)
{
	//Get width and height
	mosaicTotalWidth = ::GetWidth(size);
	mosaicTotalHeight = ::GetHeight(size);
	ratio = ComputeAspectRatio(mosaicTotalWidth, mosaicTotalHeight);
	//Store mosaic type
	mosaicType = type;

	//Store number of slots
	numSlots = GetNumSlotsForType(type);

	//Chemin avfilter : trames par slot (vide au départ) + aspect par slot
	slotFrames.resize(numSlots);
	slotKeepAspect.assign(numSlots, true);

	//Allocate sizes
	mosaicSlots = (int*)malloc(numSlots*sizeof(int));
	mosaicPos   = (int*)malloc(numSlots*sizeof(int));
	mosaicSlotsBlockingTime = (QWORD*)malloc(numSlots*sizeof(QWORD));

	//Empty them
	memset(mosaicSlots,0,numSlots*sizeof(int));
	memset(mosaicPos,0,numSlots*sizeof(int));
	memset(mosaicSlotsBlockingTime,0,numSlots*sizeof(QWORD));

	//Not changed
	mosaicChanged = false;

	//No overlay
	overlay = nullptr;

	//No vad particpant
	vadParticipant = 0;

	keepAspect = true;
}

Mosaic::~Mosaic()
{
	//If already have slot list
	if (mosaicSlots)
		//Delete it
		free(mosaicSlots);

	//If already have position list
	if (mosaicPos)
		//Delete it
		free(mosaicPos);
	//Check blocking time
	if (mosaicSlotsBlockingTime)
		//Free it
		free(mosaicSlotsBlockingTime);
		
	
	// Clean all overlays (les unique_ptr detruisent les Overlay)
	overlays.clear();
}

/************************
* SetSlot
*	Set slot participant
*************************/
int Mosaic::SetSlot(int num,int id)
{
	//Set wihtout blocking
	return SetSlot(num,id,0);
}

DWORD Mosaic::ComputeAspectRatio(DWORD imgWidth, DWORD imgHeight)
{
	if (imgHeight == 0)
		return 0;
	
	DWORD tmpratio = 1000*imgWidth/imgHeight;
	
	switch( tmpratio/10 )
	{
		case 133: // VGA
		case 122: // CIF and so on
		case 146: // SIF and so on
		    // All these definition need to be stretched to have 4:3 aspect
		    return 1333;
		    
		case 177:
		    // 16:9 aspect ratio
		    return 1777;
		    
		default:
		    // Unknown aspect ratto
		    return tmpratio;
	}
}
/************************
* SetSlot
*	Set slot participant
*************************/
int Mosaic::SetSlot(int num,int id,QWORD blockedUntil)
{
	//Check num
	Participants::iterator it;
	if (num>numSlots-1 || num<0)
		//Exit
		return Error("Slot %d not in mosaic \n", num);






	if (mosaicSlots[num] == SlotVAD && id != SlotVAD)
	{
		//Log
		Log("-SetSlot [slot=%d was vadslot.\n",num);
		vadParticipant = 0;
	}
	//Set slot to participant id
	if (id != SlotReset) mosaicSlots[num] = id;

	//If we fix a participant
	if (id>0)
	{
		//Get the id of the former participant in that slot
		int partId = mosaicPos[num];
		//Find new participant
		it = participants.find(id);
		//If it is found
		if (it!=participants.end())
		{
			//Get position
			int pos = it->second;
			//Ensure it is inside bounds and was shown
			if (pos>=0 && pos<numSlots)
			{
				//Set the old position free
				mosaicSlots[pos] = SlotFree;
				//Clean slot position
				mosaicPos[pos] = 0;
			}
			// change the output position
			it->second = num;
			//Set slot position
			mosaicPos[num] = id;
		}
		//Find old participant
		it = participants.find(partId);
		Log("-SetSlot: oldParticioant in slot %d is %d.\n", num, partId);
		//IF it was there
		if (it!=participants.end() && id != partId)
		{
			//Get next free slot for it
			it->second = GetNextFreeSlot(partId);
			Log("-SetSlot: moved old participant %d in slot %d.\n", partId, it->second);
		}
	} else if (id == SlotFree) {
		//Get the id of the participant in that slot
		int partId = mosaicPos[num];
		//Find it
		it = participants.find(partId);
		//IF it was there
		if (it!=participants.end())
			//participant is not shown, yet
			it->second = NotShown;
		//Clean slot position
		mosaicPos[num] = 0;
	} else if (id == SlotLocked) {
		//Get the id of the participant in that slot
		int partId = mosaicPos[num];
		//Find it
		it = participants.find(partId);
		//IF it was there
		if (it!=participants.end())
			//Get next free slot for it
			it->second = GetNextFreeSlot(partId);
		//Clean slot position
		mosaicPos[num] = 0;
	} else if (id == SlotVAD) {
	
		if (mosaicSlots[num] != SlotVAD)
		{
			//Get the id of the participant in that slot
			int partId = mosaicPos[num];
			//Find it
			Participants::iterator it = participants.find(partId);
			//IF it was there
			if (it!=participants.end())
				//Get next free slot for it
				it->second = GetNextFreeSlot(partId);
			//Clean slot position
			mosaicPos[num] = 0;
		}
		
	} else if (id == SlotReset) {
		if (mosaicSlots[num] == SlotFree)
		{
			int partId = mosaicPos[num];
			it = participants.find(partId);
			//IF it was there
			if (it!=participants.end())
				it->second = NotShown;
			mosaicPos[num] = 0;
		}
		else
			Log("Cannot reset slot %d. It was not a free slot but %d.\n", num,
			    mosaicSlots[num]);
	}
	

	//Set blocking time
	mosaicSlotsBlockingTime[num] = blockedUntil;

	//Evirything ok
	return 1;
}

/************************
* GetPositions
*	Get position for participant
*************************/
int* Mosaic::GetPositions()
{
	//Return them
	return mosaicPos;
}

QWORD Mosaic::GetBlockingTime(int pos)
{
	//Check if the position is fixed
	return pos>=0 && pos<numSlots ? mosaicSlotsBlockingTime[pos] : 0;
}

/************************
* GetPosition
*	Get position for participant
*************************/
int Mosaic::GetPosition(int id)
{
	//Find it
	Participants::iterator it = participants.find(id);

	//If not found
	if (it==participants.end())
		//Exit
		return NotFound;

	//Get position
	return it->second;
}

int Mosaic::HasParticipant(int id)
{
	//Find it
	Participants::iterator it = participants.find(id);

	//If not found
	if (it==participants.end())
		//Exit
		return 0;

	//We have it
	return 1;
}

int Mosaic::GetNextFreeSlot(int id)
{
	//Look in the slots
	for (int i=0;i<numSlots;i++)
	{
		//It's lock for me or it is free
		if ((id > 0 && mosaicSlots[i]==id) || (mosaicSlots[i]==0 && mosaicPos[i]==0))
		{
			//Set our position if we are a real participant
			if (id > 0) mosaicPos[i]=id;
			//Return slot
			return i;
		}
	}

	//Not slot found
	return NotShown;
}

int Mosaic::AddParticipant(int id)
{
	PartInfo info;
	//Chck if allready added
	Participants::iterator it = participants.find(id);

	//If it was
	if (it!=participants.end())
	{
		 Log ("participant id %d was already in this mosaic with pos %d.\n", id, it->second);
		//Return it
		return it->second;
	}
	
	//Not shown by default
	int pos = GetNextFreeSlot(id);

	//Set position and VAD level to 0
	participants[id] = pos;

	//Add vad info for the participant
	info.vadLevel = 0;
	info.kickable = false;
	info.eligible = false;
	
	//Set it
	partVad[id] = info;
	
	Log("-AddParticipant [id:%d,pos:%d]\n",id,pos);

	//Return it
	return pos;
}

int Mosaic::RemoveParticipant(int id)
{
	//Find it
	Participants::iterator it = participants.find(id);

	//If not found
	if (it==participants.end())
		//Exit
		return NotFound;

	//Get position
	int pos = it->second;

	//Remove it for the list
	participants.erase(it);

	//Log
	Log("-RemoveParticipant [%d,%d]\n",id,pos);

	//If  was shown
	if (pos>=0 && pos<numSlots)
	{
		//Check if it was locked
		if (mosaicSlots[pos]==id)
			//lock it
			mosaicSlots[pos] = SlotLocked;

		//Clean slot position
		mosaicPos[pos] = 0;
		//Unblock
		mosaicSlotsBlockingTime[pos] = 0;
		Clean(pos);
	}

	//Get part info
	ParticipantInfos::iterator itVad = partVad.find(id);
	//If found
	if (itVad!=partVad.end())
		//Delete it
		partVad.erase(itVad);

	std::map<int, std::unique_ptr<Overlay>>::iterator ito = overlays.find(id);
	if ( ito !=  overlays.end() )
	    //erase detruit le unique_ptr (corrige la fuite du "= NULL" precedent)
	    overlays.erase(ito);
	//Recalculate positions
	CalculatePositions();

	//Return position
	return pos;
}

int Mosaic::UpdateParticipantInfo(int id, int vadLevel)
{
	//Get participant position
	int pos = GetPosition(id);

	//Check it is in the mosaic
	if (pos==NotFound)
		//Exit
		return NotFound;

	//Get info
	ParticipantInfos::iterator it = partVad.find(id);

	//Check if present
	if (it==partVad.end())
		//Exit
		return NotFound;

	//Get info
	PartInfo &info = it->second;
	
	//Set vad level
	info.vadLevel = vadLevel;

	//IF the participant is not speaking and it is not fixed
	if (vadLevel == 0 && pos > 0 && mosaicSlots[pos] == SlotFree && id != vadParticipant)
		//Kicable
		info.kickable = true;
	else
		//Not kickable
		info.kickable = false;

	//If it is speaking and not shown
	if (vadLevel > 0 && pos == NotShown && id != vadParticipant)
		//Try to add it to the mosaic
		info.eligible = true;
	else
		//Do nothing
		info.eligible = false;

	//Check if it is movable
	if ( info.eligible || info.kickable )
		//It needs re-calculation
		return 1;
	
	//It does not need it
	return 0;
}

int Mosaic::CalculatePositions()
{
	//Get number of available slots
	int numSlots = GetNumSlots();

	Participants kickables;

	Participants::iterator it;
	ParticipantInfos::iterator it2;
	int id, vadPos;

	// Pass 1 - Build kickable slot list
	for (it2 = partVad.begin(); it2 != partVad.end(); ++it2)
	{
		id = it2->first;
	 	PartInfo &info = it2->second;
		it = participants.find(id);

		if ( info.kickable && it!=participants.end() && it->second >= 0 && it->second < numSlots)
			kickables[id] = it->second;
	}

	//Pass 2 - reshuffle
	Participants::iterator itk = kickables.begin();
	vadPos = GetVADPosition();

	for (it = participants.begin(); it != participants.end(); ++it)
	{
		bool eligible=false;
		id = it->first;
		int oldslot = it->second;
		it2 = partVad.find(id);
		if (it2 != partVad.end() )
			eligible = it2->second.eligible;

		int newslot;
		if (eligible)
		{
			// This participant is eligible. Try to get a free slot first
			newslot = GetNextFreeSlot(id);
			if (newslot == NotShown)
			{
			    // If none available select a kickable slot.
			    if ( itk != kickables.end() )
			    {
			        newslot = itk->second;
				if (newslot >=0) mosaicPos[newslot] = id;
				participants[itk->first] = NotShown; // previous partiicpant is not shown anymore.
				itk++;
			    }
			    else
			    {
			        // we cannot elect this participant. Process the next one
				continue;
			    }
			}

			// participant has been elected. Update its position now.
			participants[id] = newslot;
			Log("CalculatePosition: Elected participant %d -> slot %d.\n", id, newslot);
		
		}
		else if ( id != vadParticipant )
		{
			int oldslot = it->second;
			// if participant is not to be elected then ... do nothing. It has either been
			// kicked or remain in place (and in peace)

			if ( oldslot == NotShown )
			{
				newslot = GetNextFreeSlot(id);
				participants[id] = newslot;
				if (newslot >= 0) Log("CalculatePosition: participant %d not shown -> new slot =%d\n", id, newslot);


			}
			else if ( oldslot == vadPos )
			{
				// Special case
				// This participant was the former VAD participant and its output is set to vad slot
				// move it somewhere else
				newslot = GetNextFreeSlot(id);
				participants[id] = newslot;
				Log("CalculatePosition: participant %d old vad -> new slot =%d\n", id, newslot);
			
			}
			else
			{
			    if ( oldslot >= 0 &&  mosaicSlots[oldslot] == SlotFree)
			    {
					// participant is supposed to be shown on a free (movable) slot
					if ( mosaicPos[oldslot] != id )
					{
						 // check consistency
							 newslot = GetNextFreeSlot(id);
						 participants[id] = newslot;
					}
					else
					{
						// check if mosaic needs to be compacted
						newslot = GetNextFreeSlot(0);
						if (  newslot >= 0 && oldslot > newslot )
						{
							mosaicPos[newslot] = id;
							mosaicPos[oldslot] = 0;
							participants[id] = newslot;
							Log("CalculatePosition: moving part %d to slot %d as there is a hole oldpos = %d.\n", id, newslot , oldslot);
						}
					}
			    }
			}
		}
	}
	// Dumping positions
	for (int i=0; i< numSlots; i++)
	{
		//Log("CalculatePosition: slot #%d -> %d, part = %d.\n", i, mosaicSlots[i],  mosaicPos[i] );
		if ( mosaicSlots[i] == SlotFree )
		{
		    if ( mosaicPos[i] > 0 )
		    {
			it = participants.find(mosaicPos[i]);
			if ( it != participants.end() )
			{
				// check consistency
				if ( it->second != i )
				{
					// What should we do ?
					Log("CalculatePosition: inconsistency - slot %d contains part %d, which has pos %d\n",
					    i, mosaicPos[i], it->second);
					mosaicPos[i] = 0;
				}
			}
			else
			{
				Log("CalculatePosition: inconsistency - unknown participant referenced. Resetting.\n");
				mosaicPos[i] = 0;			    }
			}
		}
	}
	return 0;
}

int* Mosaic::GetSlots()
{
	return mosaicSlots;
}

int Mosaic::GetNumSlots()
{
	return numSlots;
}

void Mosaic::SetSlots(int *slots,int num)
{
	//Reset slot list
	memset(mosaicSlots,0,numSlots*sizeof(int));

	//If we have old slots
	if (!slots)
		return;

	//Which was bigger?
	if (num<numSlots)
		//Copy
		memcpy(mosaicSlots,slots,num*sizeof(int));
	else
		//Copy
		memcpy(mosaicSlots,slots,numSlots*sizeof(int));
}


Mosaic* Mosaic::CreateMosaic(Type type,DWORD size)
{
	//Create mosaic depending on composition
	switch(type)
	{
		case Mosaic::mosaic1x1:
		case Mosaic::mosaic2x2:
		case Mosaic::mosaic3x3:
		case Mosaic::mosaic4x4:
			//Set mosaic
			return new PartedMosaic(type,size);
		case Mosaic::mosaic1p1:
		case Mosaic::mosaic3p4:
		case Mosaic::mosaic1p7:
		case Mosaic::mosaic1p5:
		case Mosaic::mosaic1p4:
		case Mosaic::mosaic2p8:
			//Set mosaic
			return new AsymmetricMosaic(type,size);
		case mosaicPIP1:
		case mosaicPIP3:
			return new PIPMosaic(type,size);
	}

	//Type inconnu : les deux API de contrôle (XML-RPC MCU et JSR-309) transmettent
	//un entier brut du réseau casté en Mosaic::Type, sans validation. Rendre NULL
	//plutôt que lever : ce code s'exécute sous le verrou de VideoMixer, et l'ancien
	//`throw new std::runtime_error` (un POINTEUR) n'était attrapable par personne —
	//un simple type erroné du contrôleur terminait le mediaserver entier, toutes
	//conférences confondues. Les appelants testent le retour (cf. VideoMixer).
	Error("-CreateMosaic: type de composition inconnu [%d]\n",type);
	return NULL;
}

int Mosaic::SetOverlayImage(int id,const char* filename)
{
    if ( id <= 0)
    {    
	Log("-SetOverlay [%s] for mosaic\n",filename);

	//Create new one (le unique_ptr detruit l'ancien)
	overlay = std::make_unique<Overlay>(mosaicTotalWidth,mosaicTotalHeight);
	//And load it
	if(!overlay->LoadImage(filename))
		//Error
		return Error("Error loading picture image");
	//Display it (SetChanged invalide aussi le composite avfilter)
	SetChanged();
	return 1;
    }
    else
    {
	int ret = 0;
        Log("-SetOverlay [%s] for participant %d\n",filename, id);
	Participants::const_iterator it = participants.find(id);
	if (it != participants.end() )
	{
	    //Par is in the mosaic
	    Overlay * o = NULL;
	    std::map<int, std::unique_ptr<Overlay>>::iterator ito = overlays.find(id);

	    // Create overlay if neede
	    if ( ito !=  overlays.end() )
		o = ito->second.get();

	    if ( o == NULL ) o = new Overlay();

	    //Dimensionner au slot UTILE (liseré déduit, comme BuildDesc) AVANT le
	    //rendu : un Overlay neuf est en 0x0 et LoadImage refuserait de rendre
	    //(« no slot size »).
	    if (it->second >= 0 && it->second < numSlots)
	    {
		const int b = GetSlotBorder(it->second);
		o->Resize( this->GetWidth(it->second)  - 2*b,
		           this->GetHeight(it->second) - 2*b );
	    }

	    if (filename != NULL && strlen(filename) > 0)
		ret = o->LoadImage(filename);
	    else
	    {
		o->Clear();
		ret = 1;
	    }

	    if ( ito ==  overlays.end() )
	    {
		overlays[id] = std::unique_ptr<Overlay>(o);
	    }
	    //Recomposer avec (ou sans) le nouvel overlay
	    SetChanged();
	}
	else
	{
	     Log("-SetOverlay [%s]: participant not found.\n", filename);
	}
	return ret;
    }
}

int Mosaic::SetOverlaySVG(int id, const char* svg)
{
	//Nothing yet
	return false;
}
int Mosaic::ResetOverlay(int id)
{
    if ( id <= 0 )
    {
	//Log
	Log("-Reset mosaic overlay\n");
	//Reset any previous one (le unique_ptr detruit l'Overlay)
	overlay.reset();
	//Recomposer sans l'overlay
	SetChanged();
	//OK
	return 1;
    }
    else
    {
        Log("-Reset Overlay for participant %d\n", id);
	Participants::const_iterator it = participants.find(id);
	if (it != participants.end() )
	{
	    std::map<int, std::unique_ptr<Overlay>>::iterator ito = overlays.find(id);

	    if ( ito != overlays.end() )
	    {
		ito->second->Clear();
		//Recomposer sans l'overlay
		SetChanged();
		return 1;
	    }
	}
	Log("-Reset Overlay: participant not found.\n");
	return 0;
    }
}

int Mosaic::SetOverlayTXT(int id, const char* msg,int scriptCode)
{
    int res=0; 
    Participants::const_iterator it = participants.find(id);
    if (it != participants.end() )
    {
        //Par is in the mosaic
	Overlay * o = NULL;
	std::map<int, std::unique_ptr<Overlay>>::iterator ito = overlays.find(id);
	// Create overlay if neede
	if ( ito ==  overlays.end() )
	{
	   o = new Overlay();
	}
	else
	{
	    o = ito->second.get();
	}
	
        if (it->second >= 0 && it->second < numSlots )
	{
	    //Slot utile (liseré déduit), cohérent avec BuildDesc.
	    const int b = GetSlotBorder(it->second);
	    o->Resize( this->GetWidth(it->second)  - 2*b,
	               this->GetHeight(it->second) - 2*b );
	}
	if (msg != NULL)
		res =	o->RenderText(msg,scriptCode);
	    else
	        res =   o->Clear();

	if ( ito ==  overlays.end() )
	{
	   overlays[id] = std::unique_ptr<Overlay>(o);
	}
	//Recomposer avec le texte (re-rendu même taille = pas de rebuild de graphe)
	SetChanged();
   }
   return res;
}

int Mosaic::GetVADPosition()
{
	//for each slot
	for (int i=0;i<numSlots;i++)
		//Check slot
		if (mosaicSlots[i]==SlotVAD)
			//This is it
			return i;
	//Not found
	return NotFound;
}
int Mosaic::GetVADParticipant()
{
	return vadParticipant;
}

int Mosaic::SetVADParticipant(int id,QWORD blockedUntil)
{
	//Set it
	vadParticipant = id;
	
	//Find vad slot
	int pos = GetVADPosition();
	//If found
	if (pos>=0 && pos<numSlots)
	{
		//Set block time
		mosaicSlotsBlockingTime[pos] = blockedUntil;
		mosaicPos[pos] = id;
	}
	else
	{
		Log("-SetVADParticipant : there is no VAD slot defined in this mosaic.\n");
	}

	//Get vad info
	ParticipantInfos::iterator it = partVad.find(id);

	//If found
	if (it!=partVad.end())
	{
		//Get info
		PartInfo &info = it->second;
		//Participant is not kickable and not elegible
		info.kickable = false;
		info.eligible = false;
	}

	//Return vad position
	return pos;
}

bool Mosaic::IsFixed(DWORD pos)
{
	//Check if the position is fixed
	return pos>=0 && pos<numSlots ? mosaicSlots[pos]>0 : false;
}

void Mosaic::Dump()
{
	char p[16];
	char line1[1024];
	char line2[1024];

	//Empty
	*line1=0;
	*line2=0;

	for (int i=0;i<numSlots;++i)
	{
		if (i)
		{
			strcat(line1,",");
			strcat(line2,",");
		}
		sprintf(p,"%.4d",mosaicSlots[i]);
		strcat(line1,p);
		sprintf(p,"%.4d",mosaicPos[i]);
		strcat(line2,p);
	}

	Log("-MosaicSlots [%s]\n",line1);
	Log("-MosaicPos   [%s]\n",line2);

}

void Mosaic::MoveOverlays(Mosaic *other)
{
    std::map<int, std::unique_ptr<Overlay>>::iterator it;

    overlays.clear();
    for (it = other->overlays.begin(); it != other->overlays.end(); ++it)
    {
	overlays[it->first] = std::move(it->second);
	Log("-MoveOverlay: moved overlay for part %d.\n", it->first);
    }
    other->overlays.clear();

    if (other->overlay)
	this->overlay = std::move(other->overlay);

    //Recomposer avec les overlays déplacés
    SetChanged();
}

