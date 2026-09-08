/**
 * test_codec_type.cpp — le membre `type` d'un codec doit être lisible à travers
 * un pointeur de base.
 *
 * Garde-fou d'une panne vécue : `FfVideoEncoder` et `FfVideoDecoder`
 * (libmedikit/ffvideocodec.h) redéclaraient chacun un membre
 * `VideoCodec::Type type`, masquant celui, public, de leurs bases
 * `VideoEncoder`/`VideoDecoder`. Les constructeurs renseignaient donc le membre
 * DÉRIVÉ tandis que celui de la base restait non initialisé — or tout le mcu lit
 * `->type` à travers un pointeur de base :
 *
 *   videostream.cpp:810        (VideoStream::RecVideo)
 *   jsr309/VideoDecoderWorker.cpp:190
 *   rtmpparticipant.cpp:949, 961, 973
 *   FLVEncoder.cpp:404
 *
 * Le prédicat « recréer le codec si le type a changé » était donc toujours vrai :
 * le décodeur H264 était détruit et recréé à CHAQUE paquet RTP, le tampon de
 * dépaquetisation ne survivait jamais jusqu'à une trame complète, et aucune image
 * n'était décodée (mosaïque vide, surface unie chez les endpoints). L'audio était
 * épargné car ffaudiocodec.h ne masque pas `type`.
 *
 * Ces tests parcourent les codecs que la machine déclare supportés (donc pas de
 * dépendance à un codec optionnel) et vérifient l'invariant côté base.
 */
#include <gtest/gtest.h>

#include <memory>

#include "log.h"
#include "audio.h"
#include "video.h"

namespace {

// Un codec peut être déclaré supporté mais indisponible à la construction ;
// on n'échoue que si la fabrique rend un objet dont le type est incohérent.
template <typename Codec, typename Base>
void ExpectTypeVisibleFromBase(Codec codec, Base* obj, const char* name, const char* role)
{
	if (!obj)
	{
		GTEST_LOG_(INFO) << "codec " << name << " (" << role << ") non instanciable, ignore";
		return;
	}
	// LA vérification : lecture de `type` via le pointeur de BASE.
	EXPECT_EQ(codec, obj->type)
		<< role << " " << name << " : membre 'type' masque ou non initialise "
		<< "(cf. en-tete de ce fichier)";
	delete obj;
}

} // namespace

TEST(CodecType, VideoDecodersExposeTheirTypeThroughBase)
{
	const std::vector<VideoCodec::Type>& codecs = VideoCodecFactory::GetSupportedCodecs();
	ASSERT_FALSE(codecs.empty()) << "aucun codec video supporte : environnement casse";

	for (VideoCodec::Type c : codecs)
		ExpectTypeVisibleFromBase(c, VideoCodecFactory::CreateDecoder(c),
		                          VideoCodec::GetNameFor(c), "decodeur video");
}

TEST(CodecType, VideoEncodersExposeTheirTypeThroughBase)
{
	const std::vector<VideoCodec::Type>& codecs = VideoCodecFactory::GetSupportedCodecs();
	ASSERT_FALSE(codecs.empty());

	for (VideoCodec::Type c : codecs)
		ExpectTypeVisibleFromBase(c, VideoCodecFactory::CreateEncoder(c),
		                          VideoCodec::GetNameFor(c), "encodeur video");
}

TEST(CodecType, AudioCodecsExposeTheirTypeThroughBase)
{
	const std::vector<AudioCodec::Type>& codecs = AudioCodecFactory::GetSupportedCodecs();
	ASSERT_FALSE(codecs.empty()) << "aucun codec audio supporte : environnement casse";

	for (AudioCodec::Type c : codecs)
	{
		ExpectTypeVisibleFromBase(c, AudioCodecFactory::CreateDecoder(c),
		                          AudioCodec::GetNameFor(c), "decodeur audio");
		ExpectTypeVisibleFromBase(c, AudioCodecFactory::CreateEncoder(c),
		                          AudioCodec::GetNameFor(c), "encodeur audio");
	}
}

// Reproduit littéralement le prédicat de VideoStream::RecVideo (videostream.cpp:810)
// pour un flux mono-codec : il doit être FAUX dès le second paquet, sinon le
// décodeur est recréé à chaque paquet RTP et plus aucune trame n'est décodée.
TEST(CodecType, DecoderIsNotRecreatedOnEveryPacket)
{
	const VideoCodec::Type type = VideoCodec::H264;
	std::unique_ptr<VideoDecoder> decoder(VideoCodecFactory::CreateDecoder(type));
	ASSERT_NE(nullptr, decoder.get()) << "le decodeur H264 doit etre disponible";

	// Le paquet suivant porte le même codec : aucune recréation attendue.
	EXPECT_FALSE(type != decoder->type)
		<< "le predicat de recreation du decodeur est toujours vrai : "
		<< "la video RTP ne peut pas fonctionner";
}
