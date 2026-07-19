/**
 * test_amf.cpp — round-trips AMF0 (amf.h/amf.cpp).
 *
 * AMF0 est la sérialisation des messages de commande/metadata RTMP. On vérifie
 * ici le contrat Serialize → AMFParser::Parse pour chaque type porté par le mcu
 * (Number, Boolean, String, Object, Null) ainsi que les briques de longueur
 * U16Parser/U32Parser. C'est la couche que le harnais historique rtmptest.cpp
 * exerçait indirectement en rejouant une capture ; on la teste ici de façon
 * autonome et déterministe.
 */
#include <gtest/gtest.h>

#include "amf.h"

namespace {

// Sérialise un AMFData, le reparse via AMFParser, et rend l'objet reconstruit.
// Le pointeur retourné appartient à l'AMFParser fourni (ne pas le delete).
AMFData* RoundTrip(AMFData& in, AMFParser& parser)
{
	BYTE buffer[4096];
	DWORD len = in.Serialize(buffer, sizeof(buffer));
	EXPECT_GT(len, 0u);
	EXPECT_EQ(len, in.GetSize());

	DWORD consumed = parser.Parse(buffer, len);
	EXPECT_EQ(consumed, len);
	EXPECT_TRUE(parser.IsParsed());
	return parser.GetObject();
}

} // namespace

TEST(Amf, NumberRoundTrip)
{
	AMFNumber in(3.14159);
	AMFParser parser;
	AMFData* out = RoundTrip(in, parser);
	ASSERT_NE(out, nullptr);
	ASSERT_EQ(out->GetType(), AMFData::Number);
	EXPECT_NEAR(((AMFNumber*)out)->GetNumber(), 3.14159, 1e-9);
}

TEST(Amf, NumberIntegralValues)
{
	// Valeurs positives ET négatives : le signe est correctement restitué depuis
	// la correction d'AMFNumber::GetNumber (cf. TEST.md). Zéro reste à part (quirk).
	for (double v : {1.0, 2.0, 65536.0, 1000000.0, 3191.0, -1.0, -123456.0, -0.5})
	{
		AMFNumber in(v);
		AMFParser parser;
		AMFData* out = RoundTrip(in, parser);
		ASSERT_NE(out, nullptr);
		ASSERT_EQ(out->GetType(), AMFData::Number);
		EXPECT_DOUBLE_EQ(((AMFNumber*)out)->GetNumber(), v);
	}
}

// Aller-retour exact des AMFNumber NÉGATIFS. Non-régression du correctif de signe
// d'AMFNumber::GetNumber : `value` étant un uint64_t (décalage logique), l'ancien
// facteur `(value>>63|1)` valait toujours 1 et perdait le signe ; il faut porter la
// mantisse dans un int64_t signé avant de la négativer (cf. TEST.md).
TEST(Amf, NumberNegativeRoundTrip)
{
	for (double v : {-1.0, -2.5, -123456.0, -0.001, -1e6})
	{
		AMFNumber in(v);
		AMFParser parser;
		AMFData* out = RoundTrip(in, parser);
		ASSERT_NE(out, nullptr);
		ASSERT_EQ(out->GetType(), AMFData::Number);
		EXPECT_DOUBLE_EQ(((AMFNumber*)out)->GetNumber(), v);
	}
}

// Test de CARACTÉRISATION d'un défaut préexistant NON corrigé : SetNumber(0) stocke
// des bits nuls, mais GetNumber() n'a pas de cas spécial zéro et reconstruit
// ldexp(2^52, -1075) ≈ 2^-1023 (dénormal minuscule) ≠ 0.0. Un AMFNumber valant 0 ne
// fait donc PAS un aller-retour exact. Ce test épingle le comportement actuel ; s'il
// se met à échouer, c'est que le bug a été corrigé (remplacer alors par un
// EXPECT_DOUBLE_EQ(decoded, 0.0)). Voir TEST.md.
TEST(Amf, NumberZeroDecodeQuirk)
{
	AMFNumber in(0.0);
	AMFParser parser;
	AMFData* out = RoundTrip(in, parser);
	ASSERT_NE(out, nullptr);
	double decoded = ((AMFNumber*)out)->GetNumber();
	EXPECT_NE(decoded, 0.0) << "le décodage de zéro a peut-être été corrigé "
				   "(attendu : EXPECT_DOUBLE_EQ(decoded, 0.0))";
	EXPECT_NEAR(decoded, 0.0, 1e-300); // extrêmement proche de 0, mais pas 0
}

TEST(Amf, BooleanRoundTrip)
{
	for (bool v : {true, false})
	{
		AMFBoolean in(v);
		AMFParser parser;
		AMFData* out = RoundTrip(in, parser);
		ASSERT_NE(out, nullptr);
		ASSERT_EQ(out->GetType(), AMFData::Boolean);
		EXPECT_EQ(((AMFBoolean*)out)->GetBoolean(), v);
	}
}

TEST(Amf, StringRoundTrip)
{
	AMFString in(L"onMetaData");
	AMFParser parser;
	AMFData* out = RoundTrip(in, parser);
	ASSERT_NE(out, nullptr);
	ASSERT_EQ(out->GetType(), AMFData::String);
	EXPECT_EQ(((AMFString*)out)->GetWString(), std::wstring(L"onMetaData"));
}

TEST(Amf, StringUtf8RoundTrip)
{
	// Caractères non-ASCII : valide le passage par UTF8Parser (medkit).
	AMFString in(L"café éèà");
	AMFParser parser;
	AMFData* out = RoundTrip(in, parser);
	ASSERT_NE(out, nullptr);
	ASSERT_EQ(out->GetType(), AMFData::String);
	EXPECT_EQ(((AMFString*)out)->GetWString(), std::wstring(L"café éèà"));
}

TEST(Amf, NullRoundTrip)
{
	AMFNull in;
	AMFParser parser;
	AMFData* out = RoundTrip(in, parser);
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(out->GetType(), AMFData::Null);
}

TEST(Amf, ObjectRoundTrip)
{
	// Objet de commande "connect" typique.
	AMFObject in;
	in.AddProperty(L"app", L"live");
	in.AddProperty(L"flashVer", L"FMLE/3.0");
	in.AddProperty(L"audioCodecs", (double)3191.0);
	in.AddProperty(L"videoCodecs", (double)252.0);
	in.AddProperty(L"fpad", false);

	AMFParser parser;
	AMFData* out = RoundTrip(in, parser);
	ASSERT_NE(out, nullptr);
	ASSERT_EQ(out->GetType(), AMFData::Object);

	AMFObject* obj = (AMFObject*)out;
	ASSERT_TRUE(obj->HasProperty(L"app"));
	EXPECT_EQ((std::wstring)obj->GetProperty(L"app"), std::wstring(L"live"));
	ASSERT_TRUE(obj->HasProperty(L"flashVer"));
	EXPECT_EQ((std::wstring)obj->GetProperty(L"flashVer"), std::wstring(L"FMLE/3.0"));
	ASSERT_TRUE(obj->HasProperty(L"audioCodecs"));
	EXPECT_DOUBLE_EQ((double)obj->GetProperty(L"audioCodecs"), 3191.0);
	ASSERT_TRUE(obj->HasProperty(L"fpad"));
	EXPECT_FALSE((bool)obj->GetProperty(L"fpad"));
}

// --- Briques de longueur --------------------------------------------------
TEST(Amf, U16ParserRoundTrip)
{
	for (WORD v : {(WORD)0, (WORD)1, (WORD)258, (WORD)0xFFFF})
	{
		U16Parser in;
		in.SetValue(v);
		BYTE buffer[8];
		DWORD len = in.Serialize(buffer, sizeof(buffer));
		ASSERT_EQ(len, 2u);

		U16Parser out;
		ASSERT_EQ(out.Parse(buffer, len), 2u);
		ASSERT_TRUE(out.IsParsed());
		EXPECT_EQ(out.GetValue(), v);
	}
}

TEST(Amf, U32ParserRoundTrip)
{
	for (DWORD v : {(DWORD)0, (DWORD)1, (DWORD)0x00ABCDEF, (DWORD)0xFFFFFFFF})
	{
		U32Parser in;
		in.SetValue(v);
		BYTE buffer[8];
		DWORD len = in.Serialize(buffer, sizeof(buffer));
		ASSERT_EQ(len, 4u);

		U32Parser out;
		ASSERT_EQ(out.Parse(buffer, len), 4u);
		ASSERT_TRUE(out.IsParsed());
		EXPECT_EQ(out.GetValue(), v);
	}
}

// --- Robustesse : parsing fragmenté octet par octet -----------------------
TEST(Amf, NumberParsedByteByByte)
{
	AMFNumber in(42.0);
	BYTE buffer[16];
	DWORD len = in.Serialize(buffer, sizeof(buffer));

	AMFParser parser;
	for (DWORD i = 0; i < len; ++i)
		parser.Parse(buffer + i, 1);

	ASSERT_TRUE(parser.IsParsed());
	AMFData* out = parser.GetObject();
	ASSERT_NE(out, nullptr);
	ASSERT_EQ(out->GetType(), AMFData::Number);
	EXPECT_DOUBLE_EQ(((AMFNumber*)out)->GetNumber(), 42.0);
}
