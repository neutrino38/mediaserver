/**
 * test_audiomixer.cpp — cycle de vie et propriété mémoire du mixeur audio.
 *
 * `AudioMixer` possède ses sources et ses sidebars par `unique_ptr`. Deux
 * invariants ne se lisent pas dans les signatures et sont donc gardés ici :
 * la suppression d'un sidebar détache les participants qui l'écoutaient
 * (sans quoi le thread mixeur déréférencerait un objet détruit), et le
 * sidebar par défaut n'est pas supprimable (`InitMixer`/`EndMixer` écrivent
 * dedans sans revérifier son existence).
 *
 * Ces tests s'exécutent thread mixeur EN MARCHE : les pauses laissent passer
 * plusieurs ticks de 10 ms, pour que le mixeur parcoure réellement les maps
 * pendant qu'on les mute. Ils se jouent utilement sous valgrind, qui est ce
 * qui distingue ici une libération correcte d'une double libération.
 */
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "audiomixer.h"

namespace {

//Plusieurs ticks de 10 ms : le thread mixeur parcourt vraiment les maps
static void LaisserTournerLeMixeur()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST(AudioMixerOwnership, CycleDeVieCompletSousLeThreadMixeur)
{
	AudioMixer mixer;
	ASSERT_EQ(1, mixer.Init(false, 8000));

	//Le sidebar par defaut porte l'id 0 et n'est pas supprimable
	EXPECT_EQ(0, mixer.DeleteSidebar(0));

	int sb = mixer.CreateSidebar();
	ASSERT_GT(sb, 0);

	ASSERT_TRUE(mixer.CreateMixer(1));
	ASSERT_TRUE(mixer.InitMixer(1, sb));
	EXPECT_EQ(sb, mixer.GetMixerSidebar(1));

	ASSERT_EQ(1, mixer.AddSidebarParticipant(sb, 1));
	LaisserTournerLeMixeur();
	ASSERT_EQ(1, mixer.RemoveSidebarParticipant(sb, 1));

	//Supprimer le sidebar detache les participants qui l'ecoutaient
	ASSERT_EQ(1, mixer.DeleteSidebar(sb));
	EXPECT_EQ(-1, mixer.GetMixerSidebar(1));
	LaisserTournerLeMixeur();

	EXPECT_TRUE(mixer.EndMixer(1));
	mixer.DeleteMixer(1);
	LaisserTournerLeMixeur();

	EXPECT_EQ(1, mixer.End());
}

TEST(AudioMixerOwnership, EndPuisDestructionNeLiberePasDeuxFois)
{
	AudioMixer mixer;
	ASSERT_EQ(1, mixer.Init(false, 16000));
	ASSERT_TRUE(mixer.CreateMixer(7));
	ASSERT_TRUE(mixer.InitMixer(7, 0));
	LaisserTournerLeMixeur();
	ASSERT_EQ(1, mixer.End());
	//Le destructeur s'execute a la sortie : rien ne doit etre libere deux fois
}

TEST(AudioMixerOwnership, UnIdSupprimePeutEtreRecree)
{
	AudioMixer mixer;
	ASSERT_EQ(1, mixer.Init(false, 8000));

	ASSERT_TRUE(mixer.CreateMixer(3));
	ASSERT_TRUE(mixer.InitMixer(3, 0));
	mixer.DeleteMixer(3);

	//L'entree a bien quitte la map : la recreation doit reussir
	ASSERT_TRUE(mixer.CreateMixer(3));
	ASSERT_TRUE(mixer.InitMixer(3, 0));
	LaisserTournerLeMixeur();

	EXPECT_EQ(1, mixer.End());
}

TEST(AudioMixerOwnership, LaSuppressionDUnSidebarNeDetacheQueSesAuditeurs)
{
	AudioMixer mixer;
	ASSERT_EQ(1, mixer.Init(false, 8000));

	int sb1 = mixer.CreateSidebar();
	int sb2 = mixer.CreateSidebar();
	ASSERT_NE(sb1, sb2);

	for (int id = 1; id <= 4; ++id)
	{
		ASSERT_TRUE(mixer.CreateMixer(id));
		ASSERT_TRUE(mixer.InitMixer(id, (id % 2) ? sb1 : sb2));
	}
	LaisserTournerLeMixeur();

	ASSERT_EQ(1, mixer.DeleteSidebar(sb1));
	for (int id = 1; id <= 4; ++id)
	{
		if (id % 2)
			EXPECT_EQ(-1, mixer.GetMixerSidebar(id)) << "detache de sb1";
		else
			EXPECT_EQ(sb2, mixer.GetMixerSidebar(id)) << "sb2 intact";
	}
	LaisserTournerLeMixeur();

	//End() detruit tout d'un coup, sidebar par defaut compris
	EXPECT_EQ(1, mixer.End());
}

}
