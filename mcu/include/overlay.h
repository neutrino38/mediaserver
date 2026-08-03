/* 
 * File:   overlay.h
 * Author: Sergio
 *
 * Created on 29 de marzo de 2012, 23:17
 */

#ifndef OVERLAY_H
#define	OVERLAY_H
#include "config.h"
#include "video.h"
#include <string>

class Overlay
{
public:
	Overlay(DWORD width,DWORD height);
	Overlay();
	~Overlay();
	Overlay & operator =(const Overlay& o);
	int LoadImage(const char*);
	int RenderSVG(const char*);
	int RenderText(const char*,int scriptCode);
	int Clear() { contentType = NONE; cachedPict.reset(); return 0; }
	bool HasContent() { return contentType != NONE; }
	
	/**
	 * Resize overlay to the size of the useful mosaic slot (liseré déduit).
	 * Invalide le rendu en cache si la taille change ; le re-rendu est
	 * paresseux (GetPict).
	 *
	 * @param width largeur de l'overlay (= slot utile)
	 * @param height hauteur de l'overlay (= slot utile)
	 * @return true if the overlay could be resized, false otherwise.
	 *
	 **/
	bool  Resize(DWORD width,DWORD height);

	/**
	 * Renvoie le contenu rendu enveloppé dans un Pict AV_PIX_FMT_RGBA.
	 *
	 * Sert d'entrée « overlay » au graphe avfilter des mosaïques : on fournit le
	 * RGBA natif d'ImageMagick (pas de ConvertToYUVA ici), la conversion RGBA->YUV
	 * et l'application de l'alpha sont faites par le graphe. Le Pict est mis en
	 * cache et régénéré uniquement quand le contenu ou la taille change (LoadImage,
	 * RenderSVG, RenderText, Clear, Resize l'invalident). Renvoie nullptr si
	 * l'overlay n'a rien à afficher.
	 **/
	PictPtr GetPict();


private:
	// Cache du contenu rendu enveloppé en Pict RGBA (cf. GetPict).
	// Invalidé (reset) à chaque re-rendu ou redimensionnement. Seul rendu
	// depuis la Phase 6 : les buffers YUVA du blit BYTE* ont disparu.
	PictPtr cachedPict;

	DWORD width;
	DWORD height;
	std::string content;
	enum ContentType
	{
	    NONE,
	    PICTURE_BITMAP,
	    PICTURE_VECTOR,
	    TEXT
	}
	contentType;

	int scriptCode;
	
};

#endif	/* OVERLAY_H */

