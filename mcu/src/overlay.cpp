#include "overlay.h"
#include "log.h"
#include "bitstream.h"
#include <stdlib.h>
#include <string.h>
#include <Magick++.h>
#include "amf.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
}

using Magick::Quantum;
    
// Helper function de check the language of a text string
// Script Code - ISO 15924
enum TextCharType
{
    OVRL_TEXT_COMMON = 215,
    OVRL_TEXT_KATAKANA = 411,
    OVRL_TEXT_HIRAGANA = 410,
    OVRL_TEXT_HAN  = 500,  		// Chinese
    OVTL_TEXT_ARABIC  = 160,
    OVTL_TEXT_CYRILLIC = 220,
    OVRL_TEXT_UNKNOWN=0
};

// See http://en.wikipedia.org/wiki/Unicode_block
inline enum TextCharType GetCharType(wchar_t c)
{
    if (c >=0 && c < 0x0250)
    {
        return OVRL_TEXT_COMMON;
    }
    else if ( ( c >= 0x0600 && c < 0x0780) 
              || 
	      ( c >= 0x08A0 && c < 0x0900) )
    {
        return OVTL_TEXT_ARABIC;
    }
    else if ( c >= 0x0400 && c < 0x0530 )
    {
        return OVTL_TEXT_CYRILLIC;
    }
    else if ( c >= 0x3040 && c < 0x30A0 )
    {
        return OVRL_TEXT_HIRAGANA;
    }
    else if ( c >= 0x30A0 && c < 0x3100 )
    {
        return OVRL_TEXT_KATAKANA;
    }
    else if ( c >= 0x31F0 && c < 0x3400 )
    {
         return OVRL_TEXT_KATAKANA;
    }
    else if ( c >= 0x3400 && c < 0xA000 )
    {
        return OVRL_TEXT_HAN;
    }
    else
    {
        return OVRL_TEXT_UNKNOWN;
    }
}
    
// First implementation of Asian / non europeran language
// to do: split the string into pieces and render then with separate fonts.
static enum TextCharType InferStringType(const std::wstring & str)
{
   
    std::wstring & str2 = ( std::wstring & ) str;
    std::wstring::iterator it; 
    
    TextCharType tp = OVRL_TEXT_COMMON, tp2;
    for ( it = str2.begin(); it != str2.end(); it++ )
    {
	tp2 = GetCharType(*it);
	switch ( tp2 )
	{
	    case OVRL_TEXT_UNKNOWN:
 	    case OVRL_TEXT_COMMON:
		break;

	    case OVRL_TEXT_KATAKANA:
	    case OVRL_TEXT_HIRAGANA:
		return tp2;

	    default:
		if (tp == OVRL_TEXT_COMMON) tp = tp2;
		break;


	}
    }
    return tp;
}

Overlay & Overlay::operator =(const Overlay& o)
{
    Log("-Overlay: Copy this=%p, other=%p\n",this, &o);
    contentType = o.contentType;
    this->width = o.width;
    this->height= o.height;
    content = o.content;
    Resize(width, height);
    this->scriptCode=o.scriptCode;
    return *this;
}

Overlay::Overlay()
{
	contentType = NONE;
	this->width =0;
	this->height=0;
	Resize(width, height);
	Log("-Overlay: Construct default this=%p\n",this);
	this->scriptCode=0;
}


Overlay::Overlay(DWORD width,DWORD height)
{
	contentType = NONE;
	this->width = width;
	this->height = height;
	Resize(width, height);
	Log("-Overlay: Construct this=%p\n",this);
	this->scriptCode=0;
}



bool Overlay::Resize(DWORD width,DWORD height)
{
    if ( width == 0 || height == 0 )
    {
        return false;
    }

    if ( width == this->width && height == this->height )
    {
        return true; // has not changed
    }

    //Cache invalidé : la taille change (cf. GetPict, re-rendu paresseux)
    cachedPict.reset();
    //Store values
    this->width = width;
    this->height = height;
    return true;
}

Overlay::~Overlay()
{
	Log("-Overlay: destruct this=%p\n",this);
}

// Enveloppe le rendu RGBA d'ImageMagick (octets entrelacés, 4 par pixel) dans un
// Pict AV_PIX_FMT_RGBA possédé. La conversion RGBA->YUV et l'application de
// l'alpha sont faites par le graphe avfilter des mosaïques (cf. GetPict).
static PictPtr RGBABlobToPict(const Magick::Blob& blob, int width, int height)
{
	if (width <= 0 || height <= 0)
		return nullptr;
	if (blob.length() < (size_t)(4 * width * height))
		return nullptr;

	AVFrame* f = av_frame_alloc();
	if (!f)
		return nullptr;
	f->format = AV_PIX_FMT_RGBA;
	f->width  = width;
	f->height = height;
	if (av_frame_get_buffer(f, 32) < 0)
	{
		av_frame_free(&f);
		return nullptr;
	}

	av_image_copy_plane(f->data[0], f->linesize[0],
			    (const uint8_t*) blob.data(), 4 * width, 4 * width, height);

	return std::make_shared<Pict>(f);
}

int Overlay::LoadImage(const char* filename)
{
	//Cache invalidé : le contenu va être re-rendu (cf. GetPict)
	cachedPict.reset();

	if ( filename != NULL )
	{
	    contentType = PICTURE_BITMAP;
	    content = filename;
	}
	else
	{
	    if ( contentType != PICTURE_BITMAP )
	    {
	        return Error("previous content was not picture file");
	    }
	}
	
    try
    {
	Magick::Image render(content);
	if (height == 0 || width == 0)
	    return Error("-Overlay: no slot size. Cannot render image.\n");

	//Taille EXACTE demandée ('!') : tout l'aval (RGBABlobToPict, le buffersrc
	//du graphe) suppose un blob de width x height pixels. Sans le
	//flag, zoom conserve l'aspect et rend plus petit dès que l'image n'a pas le
	//ratio du slot (slot utile hors 16:9 depuis le liseré) : blob trop court,
	//overlay silencieusement abandonné.
	Magick::Geometry exact( width, height );
	exact.aspect(true);
	render.zoom( exact );
	Magick::Blob rgbablob;
	//Forcer 8 bits/canal AVANT l'export brut : ImageMagick abaisse la profondeur
	//au minimum (1 bit pour une image unie) et l'export "RGBA" la respecte, ce qui
	//produirait un blob plus court que 4 octets/pixel (cf. RenderText qui le fait déjà).
	render.depth(8);
	render.magick("RGBA");
	render.write(&rgbablob);
    
	// Pict RGBA pour le graphe (chemin GetPict) — seul rendu depuis la Phase 6.
	cachedPict = RGBABlobToPict(rgbablob, width, height);
	contentType = PICTURE_BITMAP;
	return cachedPict ? 1 : 0;
    }
    catch ( Magick::Exception &error ) 
    {
	contentType = NONE;
	return Error("-Overlay: failed to load picture file %s: %s.\n", content.c_str(), error.what() );
    }
    catch ( std::exception &error2 ) 
    {
	contentType = NONE;
	return Error("-Overlay: failed to load picture file %s: %s.\n", content.c_str(), error2.what() );
    }

}

int Overlay::RenderSVG(const char* svg)
{
    //Cache invalidé : le contenu va être re-rendu (cf. GetPict)
    cachedPict.reset();

    if (svg != NULL)
    {
	content = svg;
	contentType = PICTURE_VECTOR;
    }
    try
    {
	Magick::Image render( Magick::Geometry(width, height) );
	Magick::Blob rgbablob( content.c_str(), content.length() );
	//8 bits/canal forcés avant export brut (même piège que LoadImage : la
	//profondeur minimale d'ImageMagick raccourcirait le blob).
	render.depth(8);
	render.magick("RGBA");
	render.write(&rgbablob);
    
	// Pict RGBA pour le graphe (chemin GetPict) — seul rendu depuis la Phase 6.
	cachedPict = RGBABlobToPict(rgbablob, this->width, this->height);
	return cachedPict ? 1 : 0;
    }
    catch ( Magick::Exception &error )
    {
	contentType = NONE;
	return Error("-Overlay: failed to load picture file %s: %s.\n", content.c_str(), error.what() );
    }
    catch ( std::exception &error2 ) 
    {
	contentType = NONE;
	return Error("-Overlay: failed to load picture file %s: %s.\n", content.c_str(), error2.what() );
    }
}

int Overlay::RenderText(const char* msg, int scriptCode)
{
//    MagickCore::SetLogEventMask("All");

    //Cache invalidé : le contenu va être re-rendu (cf. GetPict)
    cachedPict.reset();

    if (msg) content = msg;

    if (width == 0 || height == 0)
    {
	Log("-Overlay: no slot dimension. Not rendering.\n");
    }

        Magick::Blob bob;
        Magick::Image render( Magick::Geometry(width, height),
                          Magick::Color(0, 0, 0, QuantumRange) );
        Magick::Color gray(QuantumRange/4, QuantumRange/4, QuantumRange/4, QuantumRange/4);

        render.strokeColor( gray );
        render.fillColor( gray ); // Fill color
        render.strokeWidth(2);
   try
   {
        //render.draw( Magick::DrawableRectangle( 8, height - 38, width-8, height-8 ) );

	// ImageMagick++ doc is incorrect. The correct argument deffinition are
	// (x_topleft, y_toplef, x_bottomrigh, y_bottomright, radius_x, radius_y )
	
	// To do
	
	std::string text2(content);
	Magick::TypeMetric textSize;
	UTF8Parser text;	    
	text.Parse( (BYTE *) content.c_str(), content.length() );
	
	// Check if it can be printed witthout be truncated
	
	render.draw( Magick::DrawableRoundRectangle( 10, height - 38, // Center
						     width - 8,  height-8,
						     5, 5) );
        render.fillColor( "white" ); // Fill color
	if (scriptCode != 0)
		this->scriptCode=scriptCode;
	
	if (scriptCode == 0 )
		this->scriptCode = InferStringType( text.GetWString() );

	switch ( this->scriptCode )
	{
	    case OVRL_TEXT_COMMON:
		Log("-Overlay: european text. Using helvetica.\n");
		render.font("helvetica");
		render.fontPointsize(24);
		break;
		
	    case OVRL_TEXT_KATAKANA:
		Log("-Overlay: rendering Katakana. Using font Sazanami-Mincho-Regular.\n");
//		render.font("Sazanami-Mincho-Mincho-Regular");
		render.font("SazanamiMincho");
		render.fontPointsize(24);
		break;
		

	    case OVRL_TEXT_HIRAGANA:
		Log("-Overlay: rendering Hiragana. Using font SazanamiMincho.\n");
		render.font("SazanamiMincho");
		render.fontPointsize(24);
		break;
	    
	    case OVRL_TEXT_HAN:  		// Chinese
		Log("-Overlay: rendering Han. Using font ZenKaiUni.\n");
	//	render.font("AR-PL-ZenKai-Uni-Medium");
		render.font("ZenKaiUni");
		render.fontPointsize(20);
		break;
	    
	    case OVTL_TEXT_ARABIC:
	    case OVTL_TEXT_CYRILLIC:	    
	    case OVRL_TEXT_UNKNOWN:
	    default:
		contentType = NONE;
		return Error("-Overlay: character set is not supported.\n");    
        }
	
	render.depth(8);
	render.interlaceType(Magick::NoInterlace);

	render.fontTypeMetrics( text2, &textSize );
	float curwidth = textSize.textWidth();
	


	while ( curwidth >= width - 12 )
	{
	        // Text is too large to fit in, lower the font size, truncate it. Leave some space for the ...
        	render.fontPointsize(20);
		if ( text.Truncate(1) == 0)
		{
		     contentType = NONE;
		     Error("-Overlay: not enough width to render text in slot.\n");
		     return 0;
		}
		
		text.Serialize(text2);
		text2 += " ...";
		render.fontTypeMetrics( text2, &textSize );
	        curwidth = textSize.textWidth();
	}
	Log("-Overlay: will render test %s.\n", text2.c_str() );
        render.draw( Magick::DrawableText( 10, height - 14, text2 ));
        render.magick("RGBA");
        render.write(&bob);
    
	// Pict RGBA pour le graphe (chemin GetPict) — seul rendu depuis la Phase 6.
	cachedPict = RGBABlobToPict(bob, width, height);
	contentType = TEXT;
	return cachedPict ? 1 : 0;
    }
    catch ( Magick::Exception &error )
    {
        contentType = NONE;
        return Error("-Overlay: failed to render text %s: %s.\n", content.c_str(), error.what() );
    }
}


PictPtr Overlay::GetPict()
{
	// Sur cache-miss (contenu/taille invalidés, ou pas encore matérialisé après
	// operator=/Resize), re-rendre depuis contentType. Les méthodes de rendu
	// repeuplent cachedPict (RGBA). Aucun contenu => rien à afficher.
	if (!cachedPict)
	{
		switch (contentType)
		{
		    case NONE:
			return nullptr;
		    case PICTURE_BITMAP:
			LoadImage(NULL);
			break;
		    case PICTURE_VECTOR:
			RenderSVG(NULL);
			break;
		    case TEXT:
			RenderText(NULL, 0);
			break;
		    default:
			break;
		}
	}

	// cachedPict RGBA (ou nullptr si le rendu a échoué). Le graphe avfilter
	// convertit RGBA->YUV lors de la composition.
	return cachedPict;
}
