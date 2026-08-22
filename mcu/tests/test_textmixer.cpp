/**
 * test_textmixer.cpp — TextMixer : routage du texte entre participants et
 * cadencement du thread de mixage.
 *
 * Le TextMixer relaie le texte de chaque source vers les workers des AUTRES
 * participants (jamais vers sa propre source), au rythme d'un tick de 200 ms.
 * Ces tests fixent ce routage de bout en bout via l'API publique réelle :
 * injection par TextOutput::SendFrame (ce que fait le textstream à la
 * réception RTP) et lecture par TextInput::GetFrame (ce que fait le
 * textstream à l'émission). Le worker peut préfixer des étiquettes de
 * participant ([nom]) : les assertions cherchent le texte EN SOUS-CHAÎNE de
 * l'accumulé, jamais en égalité stricte.
 *
 * Ils protègent aussi la conversion du cadencement vers la primitive Wait
 * (wait-primitive-unification) : End() doit interrompre le tick au lieu d'en
 * attendre la fin (l'historique msleep(200 ms) n'était pas interruptible), et
 * un ré-Init après End doit fonctionner (le Cancel de Wait est collant : sans
 * le Reset() du ré-Init, le tick partirait en boucle folle).
 */
#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <string>

#include "textmixer.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Injecte du texte comme le ferait la réception RTP du participant `id`.
static void Speak(TextMixer& mixer, int id, const std::wstring& text)
{
	TextFrame frame(0, text);
	ASSERT_NE(mixer.GetOutput(id), (TextOutput*)NULL);
	mixer.GetOutput(id)->SendFrame(frame);
}

// Lit le TextInput du participant `id` (comme l'émission RTP) en accumulant
// les trames jusqu'à voir `expected` en sous-chaîne, ou l'échéance.
static bool ReceivedContains(TextMixer& mixer, int id, const std::wstring& expected, long deadlineMs)
{
	std::wstring acc;
	Clock::time_point t0 = Clock::now();
	TextInput* input = mixer.GetInput(id);
	if (!input)
		return false;
	while (ElapsedMs(t0) < deadlineMs)
	{
		TextFrame* frame = input->GetFrame(300);
		if (frame)
		{
			acc += frame->GetWString();
			delete frame;
		}
		if (acc.find(expected) != std::wstring::npos)
			return true;
	}
	return false;
}

// Deux participants : ce que dit l'un arrive chez l'autre (via le tick de
// mixage), jamais chez lui-même.
TEST(TextMixerSite, ForwardsTextToOtherParticipantOnly)
{
	TextMixer mixer;
	std::wstring alice = L"alice", bob = L"bob";
	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.CreateMixer(2, bob));
	ASSERT_TRUE(mixer.InitMixer(1));
	ASSERT_TRUE(mixer.InitMixer(2));

	Speak(mixer, 1, L"hola mundo");

	// Le tick est à 200 ms : large marge.
	EXPECT_TRUE(ReceivedContains(mixer, 2, L"hola mundo", 3000));

	// Jamais de retour vers la propre source (le mixage exclut son worker).
	TextFrame* echo = mixer.GetInput(1)->GetFrame(300);
	if (echo)
	{
		EXPECT_EQ(echo->GetWString().find(L"hola mundo"), std::wstring::npos);
		delete echo;
	}

	mixer.End();
}

// Le texte circule dans les deux sens.
TEST(TextMixerSite, BothDirections)
{
	TextMixer mixer;
	std::wstring alice = L"alice", bob = L"bob";
	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.CreateMixer(2, bob));
	ASSERT_TRUE(mixer.InitMixer(1));
	ASSERT_TRUE(mixer.InitMixer(2));

	Speak(mixer, 1, L"un vers deux");
	Speak(mixer, 2, L"deux vers un");

	EXPECT_TRUE(ReceivedContains(mixer, 2, L"un vers deux", 3000));
	EXPECT_TRUE(ReceivedContains(mixer, 1, L"deux vers un", 3000));

	mixer.End();
}

// CIBLE de la conversion Wait : End() interrompt le tick au lieu d'attendre
// sa fin (l'historique msleep(200 ms) faisait patienter le join d'autant).
TEST(TextMixerSite, EndIsImmediate)
{
	TextMixer mixer;
	std::wstring alice = L"alice";
	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.InitMixer(1));

	// Laisser le thread s'endormir dans son tick.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	Clock::time_point t0 = Clock::now();
	mixer.End();
	EXPECT_LT(ElapsedMs(t0), 150);	// l'historique prenait jusqu'à ~200 ms
}

// GARDE-FOU du Cancel collant : après End(), un ré-Init doit redonner un
// mixeur fonctionnel (sans le Reset() du tick dans Init, la boucle partirait
// en attente morte ou en boucle folle).
TEST(TextMixerSite, ReInitAfterEndStillMixes)
{
	TextMixer mixer;
	std::wstring alice = L"alice", bob = L"bob";
	ASSERT_TRUE(mixer.Init());
	mixer.End();

	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.CreateMixer(2, bob));
	ASSERT_TRUE(mixer.InitMixer(1));
	ASSERT_TRUE(mixer.InitMixer(2));

	Speak(mixer, 1, L"apres reinit");
	EXPECT_TRUE(ReceivedContains(mixer, 2, L"apres reinit", 3000));

	mixer.End();
}

} // namespace

// ---------------------------------------------------------------------------
// Caractérisation posée AVANT le retrait de use.h (cf. la fiche mémoire
// wait-primitive-unification et le motif décrit dans smart_pointers_plan.md) :
// ces chemins-là ne sont couverts par rien, et ce sont précisément ceux dont
// la migration change la discipline de verrou.
// ---------------------------------------------------------------------------

// Le texte PRIVÉ va au seul participant visé — et à personne d'autre.
// CreatePrivate/InitPrivate/DeletePrivate étaient les seules mutations prises
// sous le verrou LECTEUR : elles modifiaient le worker d'une source pendant que
// le thread de mixage l'itérait.
TEST(TextMixerSite, PrivateTextReachesOnlyItsTarget)
{
	TextMixer mixer;
	std::wstring alice = L"alice", bob = L"bob", moderator = L"moderateur";
	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.CreateMixer(2, bob));
	ASSERT_TRUE(mixer.InitMixer(1));
	ASSERT_TRUE(mixer.InitMixer(2));

	//Un canal privé 3 -> 1
	ASSERT_TRUE(mixer.CreatePrivate(3, 1, moderator));
	ASSERT_TRUE(mixer.InitPrivate(3));

	TextOutput* priv = mixer.GetPrivateOutput(3);
	ASSERT_NE(priv, (TextOutput*)NULL);
	TextFrame frame(0, std::wstring(L"message prive."));
	priv->SendFrame(frame);

	EXPECT_TRUE(ReceivedContains(mixer, 1, L"message prive", 3000));

	//Bob ne doit rien en voir
	TextFrame* leak = mixer.GetInput(2)->GetFrame(300);
	if (leak)
	{
		EXPECT_EQ(leak->GetWString().find(L"message prive"), std::wstring::npos);
		delete leak;
	}

	EXPECT_TRUE(mixer.EndPrivate(3));
	mixer.DeletePrivate(3);
	mixer.End();
}

// Le chemin de sortie normal (celui de MultiConf) : EndMixer puis DeleteMixer
// PENDANT que le mixage tourne. Les autres continuent de s'entendre.
TEST(TextMixerSite, EndThenDeleteMixerWhileRunningKeepsTheOthers)
{
	TextMixer mixer;
	std::wstring alice = L"alice", bob = L"bob", carol = L"carol";
	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.CreateMixer(2, bob));
	ASSERT_TRUE(mixer.CreateMixer(3, carol));
	ASSERT_TRUE(mixer.InitMixer(1));
	ASSERT_TRUE(mixer.InitMixer(2));
	ASSERT_TRUE(mixer.InitMixer(3));

	//Carol s'en va, proprement
	EXPECT_TRUE(mixer.EndMixer(3));
	EXPECT_EQ(0, mixer.DeleteMixer(3));

	Speak(mixer, 1, L"toujours la");
	EXPECT_TRUE(ReceivedContains(mixer, 2, L"toujours la", 3000));

	mixer.End();
}

// Le même départ, mais SANS EndMixer préalable : DeleteMixer détruisait le
// worker en le laissant dans la liste que parcourt le thread de mixage.
TEST(TextMixerSite, DeleteMixerWithoutEndMixerKeepsTheOthers)
{
	TextMixer mixer;
	std::wstring alice = L"alice", bob = L"bob", carol = L"carol";
	ASSERT_TRUE(mixer.Init());
	ASSERT_TRUE(mixer.CreateMixer(1, alice));
	ASSERT_TRUE(mixer.CreateMixer(2, bob));
	ASSERT_TRUE(mixer.CreateMixer(3, carol));
	ASSERT_TRUE(mixer.InitMixer(1));
	ASSERT_TRUE(mixer.InitMixer(2));
	ASSERT_TRUE(mixer.InitMixer(3));

	EXPECT_EQ(0, mixer.DeleteMixer(3));

	Speak(mixer, 1, L"encore la");
	EXPECT_TRUE(ReceivedContains(mixer, 2, L"encore la", 3000));

	mixer.End();
}
