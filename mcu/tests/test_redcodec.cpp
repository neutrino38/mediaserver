/**
 * test_redcodec.cpp — non-regression du decodeur de redondance texte
 * (RFC 2198 / T.140, cf. correctif-recorder.md).
 *
 * `RedundentCodec::Decode` deduit les pertes d'un ecart de numeros de sequence.
 * Le calcul est non signe : un numero qui ne progresse pas (duplique, reordonne,
 * ou jamais pose par l'emetteur) rendait 2^32-1 pertes, et une boucle de 4
 * milliards de caracteres de remplacement qui bloquait le thread recepteur
 * jusqu'a la fin de l'appel.
 *
 * Le cas reel : WSEndpoint renvoyait l'echo du BOM keepalive d'Asterisk sans
 * numero de sequence, donc a seq 0 a chaque fois.
 *
 *     ./tests/runtests --gtest_filter='RedCodec*'
 */
#include <gtest/gtest.h>

#include <vector>

#include "redcodec.h"
#include "rtp.h"

namespace {

static const BYTE REPLACEMENT[] = {0xEF,0xBF,0xBD};

//Compte ce qui sort du decodeur, sans rien en faire d'autre.
class FrameCounter : public TextOutput
{
public:
	virtual int SendFrame(TextFrame &frame)
	{
		total++;
		if (frame.GetLength()==sizeof(REPLACEMENT) &&
		    memcmp(frame.GetData(),REPLACEMENT,sizeof(REPLACEMENT))==0)
			replacements++;
		return 1;
	}

	DWORD total = 0;
	DWORD replacements = 0;
};

//Un paquet RED texte : en-tete RTP, `redundantCount` blocs redondants d'un
//octet, puis le bloc primaire.
std::vector<BYTE> BuildRedPacket(WORD seq, BYTE redundantCount)
{
	std::vector<BYTE> pkt;

	pkt.push_back(0x80);
	pkt.push_back(100);                       //payload type
	pkt.push_back(seq >> 8); pkt.push_back(seq & 0xFF);
	pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x10); pkt.push_back(0x00); //ts
	pkt.push_back(0xDE); pkt.push_back(0xAD); pkt.push_back(0xBE); pkt.push_back(0xEF); //ssrc

	for (BYTE i=0;i<redundantCount;i++)
	{
		//F=1, type 98, offset 10*(i+1), longueur 1
		WORD offset = 10*(i+1);
		pkt.push_back(0x80 | 98);
		pkt.push_back(offset >> 6);
		pkt.push_back((offset & 0x3F) << 2);
		pkt.push_back(1);
	}
	pkt.push_back(98);                        //en-tete du bloc primaire, F=0

	for (BYTE i=0;i<redundantCount;i++)
		pkt.push_back('r');
	pkt.push_back('X');                       //bloc primaire

	return pkt;
}

} // namespace

// Le cas de la panne : deux paquets portant le meme numero de sequence.
// Le second ne doit produire que son propre contenu, pas 2^32 marques.
TEST(RedCodec, UnNumeroDeSequenceRepeteNeProduitPasDeRafale)
{
	RedundentCodec codec;
	FrameCounter out;

	std::vector<BYTE> first = BuildRedPacket(0, 1);
	RTPRedundantPacket red1(MediaFrame::Text, first.data(), first.size());
	ASSERT_TRUE(red1.IsValid());
	ASSERT_TRUE(codec.Decode(&red1, &out));

	DWORD afterFirst = out.total;

	std::vector<BYTE> second = BuildRedPacket(0, 1);
	RTPRedundantPacket red2(MediaFrame::Text, second.data(), second.size());
	ASSERT_TRUE(red2.IsValid());
	ASSERT_TRUE(codec.Decode(&red2, &out));

	//Le primaire, et rien d'autre : aucune perte a annoncer entre un paquet et
	//lui-meme.
	EXPECT_EQ(afterFirst+1, out.total);
	EXPECT_EQ(0u, out.replacements);
}

// Un paquet reordonne (numero anterieur) ne dit rien sur les pertes non plus.
TEST(RedCodec, UnNumeroDeSequenceQuiReculeNeProduitPasDeRafale)
{
	RedundentCodec codec;
	FrameCounter out;

	std::vector<BYTE> first = BuildRedPacket(5000, 1);
	RTPRedundantPacket red1(MediaFrame::Text, first.data(), first.size());
	ASSERT_TRUE(codec.Decode(&red1, &out));

	DWORD afterFirst = out.total;

	std::vector<BYTE> second = BuildRedPacket(4990, 1);
	RTPRedundantPacket red2(MediaFrame::Text, second.data(), second.size());
	ASSERT_TRUE(codec.Decode(&red2, &out));

	EXPECT_EQ(afterFirst+1, out.total);
	EXPECT_EQ(0u, out.replacements);
}

// Un trou immense reste borne : ce qui sort tient dans le plafond, pas dans
// l'ecart de numeros de sequence.
TEST(RedCodec, UnTrouImmenseResteBorne)
{
	RedundentCodec codec;
	FrameCounter out;

	std::vector<BYTE> first = BuildRedPacket(1, 1);
	RTPRedundantPacket red1(MediaFrame::Text, first.data(), first.size());
	ASSERT_TRUE(codec.Decode(&red1, &out));

	out.total = 0;
	out.replacements = 0;

	std::vector<BYTE> second = BuildRedPacket(60000, 1);
	RTPRedundantPacket red2(MediaFrame::Text, second.data(), second.size());
	ASSERT_TRUE(codec.Decode(&red2, &out));

	//Les marques de perte sont plafonnees ; le paquet reste par ailleurs
	//decode (redondance recuperee + primaire).
	EXPECT_LE(out.replacements, 16u);
	EXPECT_LE(out.total, 32u);
	EXPECT_GT(out.replacements, 0u);
}

// Le cas nominal ne doit pas avoir change : un seul paquet perdu, recupere par
// le bloc redondant, donc aucune marque de perte.
TEST(RedCodec, UnePerteRecuperableNeProduitPasDeMarque)
{
	RedundentCodec codec;
	FrameCounter out;

	std::vector<BYTE> first = BuildRedPacket(10, 1);
	RTPRedundantPacket red1(MediaFrame::Text, first.data(), first.size());
	ASSERT_TRUE(codec.Decode(&red1, &out));

	out.total = 0;
	out.replacements = 0;

	//Le paquet 11 manque, le paquet 12 porte sa copie redondante.
	std::vector<BYTE> second = BuildRedPacket(12, 1);
	RTPRedundantPacket red2(MediaFrame::Text, second.data(), second.size());
	ASSERT_TRUE(codec.Decode(&red2, &out));

	EXPECT_EQ(0u, out.replacements);
	EXPECT_EQ(2u, out.total);           //le bloc redondant, puis le primaire
}
