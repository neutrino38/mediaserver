###########################################
# Fichero de configuracion
###########################################
MACHINE := $(shell uname -m)

ifeq ($(MACHINE), x86_64)
libdir=lib64
endif
ifeq ($(MACHINE), i686)
libdir=lib
endif

LOG             = yes
DEBUG           = yes
FLASHSTREAMER   = no
ADDLIBPATH      = ../staticdeps
MEDKITDIR       = ../third_party/fontventa/libmedikit
# libbfcp est desormais un sous-module bati in-tree (comme libmedkit) : en-tetes
# et archive pris dans l'arbre du sous-module, plus de dependance a /opt/ives.
BFCPDIR         = ../third_party/libbfcp
BFCPINCLUDE     = $(BFCPDIR)/include
# libmedkit (sous-module) est desormais la source unique des codecs : liaison
# inconditionnelle de libmedkit.a. Le mode sans-medkit a ete supprime (les .o de
# codecs repris par medkit ont ete retires de OBJS). Voir integration_libmedkit.md.
PROTOC		= ../staticdeps/bin/protoc
#DEBUG
#OPTS=-msse -msse2 -msse3 -D__SSE2__
OPTS=-std=gnu++17 -Werror=return-type -Werror=delete-non-virtual-dtor -Werror=int-to-pointer-cast

ifeq ($(GPROF),yes)
        OPTS+= -g -O0 -pg
        LDFLAGS +=  -pg
endif

ifeq ($(DEBUG),yes)
	TAG=debug
	OPTS+= -g -O0
	BFCPLIBS=$(BFCPDIR)/lib/libbfcpdbg.a
else
	OPTS+= -O3
	TAG=release
	BFCPLIBS=$(BFCPDIR)/lib/libbfcprel.a
endif

#LOG
ifeq ($(LOG),yes)
	OPTS+= -DLOG_
endif

############################################
#Directorios
############################################
BUILD = media/build/$(TAG)
BIN   = ../bin/$(TAG)

############################################
#Objetos
############################################
# G711 fourni par libmedkit (PCMUEncoder/PCMAEncoder, FfAudioEncoder). Repertoire mcu supprime.
# H263/H263+/MPEG4 entierement fournis par libmedkit (FfVideoDecoder/FfVideoEncoder). Repertoire mcu supprime.

JSR309DIR=jsr309
JSR309OBJ=JSR309Event.o WSEndpoint.o RTPEndpoint.o Player.o Endpoint.o  MediaSession.o \
    JSR309Manager.o RTPMultiplexer.o xmlrpcjsr309.o AudioDecoderWorker.o \
    AudioEncoderWorker.o AudioMixerResource.o VideoDecoderWorker.o VideoEncoderWorker.o \
    VideoMixerResource.o RTPMultiplexerSmoother.o Recorder.o AudioTranscoder.o Joinable.o


# H264 entierement fourni par libmedkit (encoder, decoder, depacketizer). Repertoire mcu supprime.

# Speex fourni par libmedkit (FfAudioEncoder/Decoder, ffmpeg AV_CODEC_ID_SPEEX). Repertoire mcu supprime.


DOCSHARINGOBJ= stunmessage.o crc32calc.o remoteratecontrol.o uploadhandler.o http.o appmixer.o

MOTELIDIR=moteli
MOTELIOBJ=rabbitmqmcu.o mcu.pb.o rabbitmqserver.o

OBJS=httpparser.o websocketserver.o websocketconnection.o websockettransport.o mcu.o multiconf.o rtpparticipant.o rtmpparticipant.o videomixer.o audiomixer.o xmlrpcserver.o xmlhandler.o xmlstreaminghandler.o statushandler.o xmlrpcmcu.o rtpsession.o audiostream.o videostream.o pipeaudioinput.o pipeaudiooutput.o pipevideoinput.o pipevideooutput.o
# logo.o : fourni par libmedkit.a (medkit/logo.h). mcu ne compile plus son propre
# logo.cpp pour eviter la double definition au link (picturestreamer/mp4writer de
# libmedkit tirent la classe Logo de l'archive).
OBJS+=mosaic.o mosaiccompositor.o partedmosaic.o asymmetricmosaic.o pipmosaic.o overlay.o amf.o rtmpmessage.o rtmpchunk.o rtmpstream.o rtmpconnection.o  rtmpserver.o broadcaster.o broadcastsession.o rtmpflvstream.o flvrecorder.o FLVEncoder.o xmlrpcbroadcaster.o mediagateway.o mediabridgesession.o 
OBJS+=xmlrpcmediagateway.o textmixer.o textmixerworker.o textstream.o participanttextws.o pipetextinput.o pipetextoutput.o mp4player.o mp4streamer.o audioencoder.o audiodecoder.o textencoder.o mp4recorder.o rtmpmp4stream.o rtmpnetconnection.o rtmpclientconnection.o avcdescriptor.o RTPSmoother.o rtp.o fecdecoder.o sidebar.o \
      vad.o dtls.o VideoTranscoder.o remoterateestimator.o eventstreaminghandler.o videopipe.o redcodec.o shareddocmixer.o mcustatushandler.o dtmfmessage.o

# ipaddress.o : type d'adresse commun v4/v6 (include/ipaddress.h), socle du
# chantier IPv6. Sans dependance sur le reste du mcu : il ne tire que la libc.
OBJS+=ipaddress.o

OBJS+= $(DOCSHARINGOBJ) $(JSR309OBJ)
TARGETS=mcu rtmptest mediagw

OBJS2=mcu.o multiconf.o rtpparticipant.o rtmpparticipant.o videomixer.o audiomixer.o xmlrpcserver.o xmlhandler.o xmlstreaminghandler.o statushandler.o xmlrpcmcu.o rtpsession.o audiostream.o videostream.o pipeaudioinput.o pipeaudiooutput.o pipevideoinput.o pipevideooutput.o 
OBJS2+=overlay.o amf.o rtmpmessage.o rtmpchunk.o rtmpstream.o rtmpconnection.o  rtmpserver.o broadcaster.o broadcastsession.o rtmpflvstream.o flvrecorder.omediagateway.o mediabridgesession.o mcustatushandler.o
OBJS2+=xmlrpcmediagateway.o textstream.o pipetextinput.o pipetextoutput.o audioencoder.o audiodecoder.o textencoder.o rtmpnetconnection.o rtmpclientconnection.o avcdescriptor.o RTPSmoother.o rtp.o fecdecoder.o vad.o 
OBJS2+=remoterateestimator.o eventstreaminghandler.o VideoTranscoder.o videopipe.o redcodec.o
OBJS2+=ipaddress.o
OBJS2+= $(JSR309OBJ) $(DOCSHARINGOBJ)

ifeq ($(FLASHSTREAMER),yes)
	OBJS+= flash.o xmlrpcflash.o 
	OBJSFS   = flashstreamer.o FlashPlayer.o FlashSoundHandler.o $(OBJS)
	OBJSFSCLIENT = xmlrpcclient.o xmlrpcflashclient.o
	TARGETS += flashstreamer flashclient testflash
endif

# VAD via le paquet systeme webrtc-audio-processing (module APM).
VADINCLUDE = $(shell pkg-config --cflags webrtc-audio-processing)
VADLD = $(shell pkg-config --libs webrtc-audio-processing)
OPTS+= -DVADWEBRTC

ifeq ($(MOTELI),yes)
	OPTS+= -DMOTELI=yes
	OBJS+= $(MOTELIOBJ)
	LDFLAGS += -Wl,-Bstatic -lamqpcpp -lrabbitmq -lprotobuf -Wl,-Bdynamic
endif

OBJSMCU = $(OBJS) main.o
OBJSRTMPTEST = $(OBJS) rtmptest.o
OBJSWSTEST = $(OBJS) wstest.o

BUILDOBJSMCU = $(addprefix $(BUILD)/,$(OBJSMCU))
BUILDOBJSRTMPTEST= $(addprefix $(BUILD)/,$(OBJSRTMPTEST))
BUILDOBJSWSTEST= $(addprefix $(BUILD)/,$(OBJSWSTEST))
BUILDOBJSMEDIAGW= $(addprefix $(BUILD)/,$(OBJS2))
BUILDOBJSFS= $(addprefix $(BUILD)/,$(OBJSFS)) 
BUILDOBJSFSCLIENT= $(addprefix $(BUILD)/,$(OBJSFSCLIENT))

###################################
#Compilacion condicional
###################################
VPATH  =  %.o $(BUILD)/
VPATH +=  %.c src/
VPATH +=  %.cpp src/
VPATH +=  %.cpp src/$(JSR309DIR)

ifeq ($(MOTELI),yes)
    VPATH +=  %.cpp src/$(MOTELIDIR)
    VPATH +=  %.cc src/$(MOTELIDIR)
    VPATH +=  %.proto src/$(MOTELIDIR)
endif

# ImageMagick : Magick++ 7 (AlmaLinux 9) via pkg-config. Fournit le bon -I
# (ImageMagick-7), les -DMAGICKCORE_* requis et les libs Magick++-7.Q16HDRI.
MAGICKCFLAGS    := $(shell pkg-config --cflags Magick++)
MAGICKLIBS      := $(shell pkg-config --libs Magick++)

INCLUDE+= -Iinclude/ -I$(ADDLIBPATH)/include $(VADINCLUDE) $(MAGICKCFLAGS) -I$(BFCPINCLUDE)

###################################
# libmedkit + ffmpeg, par pkg-config
###################################
# Le sous-module publie libmedkit.pc a la racine de son arbre — chemins absolus
# vers le clone, motif « uninstalled » des autotools — et y declare ffmpeg en
# Requires:, ffvideocodec.h incluant <libavcodec/avcodec.h>. Ses -I et ses -l
# arrivent donc avec : le mcu n'ecrit plus /usr/include/ffmpeg et n'enumere plus
# les -lav*. Batir contre un autre ffmpeg ne demande alors qu'un
# PKG_CONFIG_PATH, ce dont le portage ffmpeg 9 a besoin (voir, a la racine,
# ffmpeg9_migration_plan.md).
#
# Trois modules restent en propre au mcu, que libmedkit n'utilise pas :
# libavfilter (mosaiques), libavdevice et libpostproc.
MEDKITPCDIR = $(abspath $(MEDKITDIR))
PKGMODULES  = libmedkit libavfilter libavdevice libpostproc
PKGCFLAGS  := $(shell PKG_CONFIG_PATH=$(MEDKITPCDIR):$$PKG_CONFIG_PATH pkg-config --cflags $(PKGMODULES) 2>/dev/null)
PKGLIBS    := $(shell PKG_CONFIG_PATH=$(MEDKITPCDIR):$$PKG_CONFIG_PATH pkg-config --libs   $(PKGMODULES) 2>/dev/null)

# libmedkit.pc n'existe qu'une fois le sous-module bati : l'ordre est garanti
# par install.ksh, pas par un `make` lance a la main sur un arbre neuf. Repli
# explicite et bruyant, plutot que des flags vides dont l'erreur — un en-tete
# introuvable — n'aurait aucun rapport visible avec sa cause.
ifeq ($(strip $(PKGLIBS)),)
$(warning pkg-config n'a pas resolu [$(PKGMODULES)] : repli sur les chemins en dur.)
$(warning Batir d'abord le sous-module : make -C $(MEDKITDIR) all ASTERISK=no)
PKGCFLAGS := -I$(MEDKITDIR) -I/usr/include/ffmpeg
PKGLIBS   := $(MEDKITDIR)/libmedkit.a -lavcodec -lswscale -lavformat -lavutil -lswresample -lavfilter -lavdevice -lpostproc
endif

# Les -I de libmedkit passent AVANT -Iinclude/ : les en-tetes du sous-module
# doivent primer, pour que les inclusions <g722/...> / <amr/...> visent le codec
# medkit et non son homonyme dans mcu/src.
INCLUDE := $(PKGCFLAGS) $(INCLUDE)

LDFLAGS+= $(BFCPLIBS) -L$(ADDLIBPATH)/lib
# libmedkit.a d'abord, puis ce dont elle depend : pour une archive, l'ordre du
# lien decide. pkg-config rend deja -lmedkit avant les -lav*.
LDFLAGS+= $(PKGLIBS)

ifeq ($(MOTELI),yes)
	LDFLAGS += -lamqpcpp -lrabbitmq -lprotobuf
endif

# Lien dynamique sur les paquets systeme AlmaLinux 9, sans alternative. Les
# branches DISTRO (fc / el5 / el6) et FEWSTATICDEPS ont ete supprimees : la
# detection ne reconnaissait meme pas AlmaLinux — elle tombait dans le `else`,
# seul chemin reellement emprunte depuis le portage — et le spec est reserve a
# AlmaLinux 9. Les codecs que ces branches liaient en direct (-lx264, -lvpx,
# -lspeex, -lfdk-aac, -lg722_1, -lopencore-amr*) arrivent tous par libavcodec,
# via libmedkit.
LDFLAGS += -lsrtp2 -lmp4v2 $(MAGICKLIBS) -lbz2 -ldl -lz

# xmlrpc-c : paquet systeme (xmlrpc-c-devel, depot crb) lie dynamiquement, bati
# avec le backend libxml2 — xmlParseChunk vient donc de la libxml2 systeme
# (-lxml2), et non des archives expat libxmlrpc_xmlparse/xmltok. OpenSSL : libs
# dynamiques du systeme, plus de build openssl dans staticdeps.
LDXMLFLAGS+= -lxmlrpc_abyss -lxmlrpc_server -lxmlrpc -lxmlrpc_util -lxml2 -lssl -lcrypto -lpthread

LDFLAGS+= $(LDXMLFLAGS)

#For abyss
OPTS 	+= -D_UNIX -D__STDC_CONSTANT_MACROS -fPIC

CFLAGS  += $(INCLUDE) $(OPTS)
CXXFLAGS+= $(INCLUDE) $(OPTS)

# Auto-dependances (evite les crashs par ABI/layout obsolete quand un .h change) :
# gcc emet un .d par objet listant TOUS les headers inclus ; -MP ajoute des cibles
# bidon pour les headers supprimes ; -MT $@ aligne la cible du .d sur le nom nu
# utilise par make (la sortie reelle va dans $(BUILD)). Les .d sont inclus en bas.
DEPFLAGS = -MMD -MP -MF $(BUILD)/$(@:.o=.d) -MT $@

mcu.pb.cc: mcu.proto
	$(PROTOC) --cpp_out=$(PWD) src/moteli/mcu.proto

rabbitmqmcu.o: rabbitmqmcu.cpp rabbitmqmcu.h mcu.pb.cc
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $(BUILD)/$@

mcu.pb.o: mcu.pb.cc
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -I$(PWD) -c $< -o $(BUILD)/$@

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $(BUILD)/$@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $(BUILD)/$@

############################################
#Targets
############################################
all: mkdirs $(TARGETS) 

# Les repertoires de sortie sont des cibles-fichiers a part entiere : les cibles
# qui en ont besoin les prennent en prerequis ORDER-ONLY (apres la barre |), de
# sorte que leur mtime — qui change a chaque fichier ecrit dedans — n'entre pas
# dans la comparaison de dates. C'est ce qui evite le relien systematique.
$(BUILD) $(BIN):
	mkdir -p $@

# `mkdirs` reste pour les cibles pas encore converties (all, wstest) : il ne fait
# plus le mkdir lui-meme, il le delegue aux regles ci-dessus.
mkdirs: | $(BUILD) $(BIN)

clean:
	rm -f $(BUILDOBJSMCU)
	rm -f $(BUILDOBJSFS)
	rm -f "$(BIN)/mcu"
	rm -f "$(BIN)/flashstreamer"
	rm -f $(BUILD)/*.d

install:
	cp $(BUILD)/mcu $(TARGET)/mediaserver

############################################
#MCU
############################################

# `mcu` n'est qu'un alias : le vrai produit est $(BIN)/mcu, et c'est LUI qui doit
# porter la regle. Ecrire `mcu: ...` avec une recette qui produit $(BIN)/mcu
# laissait make chercher un fichier `mcu` dans ce repertoire, ne pas le trouver,
# et donc RELIER a chaque appel meme sans un seul objet recompile.
# Les prerequis restent les noms nus des objets : VPATH les resout vers $(BUILD),
# et c'est la seule forme que la regle de motif %.o sait construire sur un arbre
# neuf (cf. DEPFLAGS -MT $@, qui nomme aussi les cibles a nu).
.PHONY: mcu
mcu: $(BIN)/mcu

$(BIN)/mcu: $(OBJSMCU) | $(BUILD) $(BIN)
	$(CXX) -o $@ $(BUILDOBJSMCU) $(LDFLAGS) $(VADLD)

rtmptest: $(OBJSRTMPTEST)
	$(CXX) -o $(BIN)/$@ $(BUILDOBJSRTMPTEST) $(LDFLAGS)

wstest: mkdirs $(OBJSWSTEST)
	$(CXX) -o $(BIN)/$@ $(BUILDOBJSWSTEST) $(LDFLAGS) $(VADLD)

mediagw:  $(OBJS2) maingw.o
	$(CXX) -o $(BIN)/$@ $(OBJSMEDIAGW) maingw.o $(LDFLAGS)
	
flashstreamer: $(OBJSFS) $(OBJS)
	g++ $(GNASHLD) -o $(BIN)/$@ $(BUILDOBJSFS) $(BUILDOBJS) $(GNASHBASE)/backend/.libs/libgnashagg.a  /usr/lib/libagg.a  $(LDFLAGS)


############################################
# Tests automatises (GoogleTest, cf. mcu/tests/README.md)
############################################
# Suite gtest liee contre les objets du mcu ($(OBJS), soit tout sauf main.o /
# rtmptest.o / wstest.o) + gtest systeme. gtest_main fournit main(). REMPLACE les
# harnais manuels rtmptest.cpp (couche chunk RTMP) et wstest.cpp (serveur d'echo
# WebSocket) par des tests autonomes et assertifs.
# Usage : make check   (build + execution)
#         make tests   (build seul -> tests/runtests)
# NB : PAS de -lgtest_main. La suite fournit son propre main() (tests/test_env.cpp)
# car libwebrtc_audio_processing.so (VAD) exporte un symbole `main` parasite qui,
# .so contre .so, l'emportait sur celui de libgtest_main dans la resolution
# dynamique. Un main() defini dans l'executable est une definition forte.
GTEST_CFLAGS := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LIBS   := $(shell pkg-config --libs gtest 2>/dev/null)
TESTSRCS     := $(wildcard tests/*.cpp)
BUILDOBJSTEST = $(addprefix $(BUILD)/,$(OBJS))
# Fixture pcap RTP/RTCP versionnee ; son chemin absolu est injecte (surchargeable :
# make check TEST_PCAP=/chemin/vers.pcap).
TEST_PCAP    ?= $(abspath tests/fixtures/rtp_rtcp.pcap)

# $(BUILD) en prerequis ORDER-ONLY, pas `mkdirs` : celui-ci n'etant pas un fichier,
# il etait toujours plus recent que tests/runtests et forcait la recompilation de
# TOUTE la suite gtest a chaque `make check`, meme sans modification.
tests/runtests: $(OBJS) $(TESTSRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) -DTEST_PCAP_FILE='"$(TEST_PCAP)"' \
		$(TESTSRCS) $(BUILDOBJSTEST) \
		-o tests/runtests $(LDFLAGS) $(VADLD) $(GTEST_LIBS)

tests: tests/runtests

check: tests/runtests
	./tests/runtests

# Suite IPv6 (tag :ipv6, cf. tests/test_ipv6.cpp et ipv6.md a la racine).
# ATTENDUE EN ECHEC tant qu'IPv6 n'est pas implemente : elle n'est PAS jouee par
# `make check`. Les tests sont prefixes DISABLED_ (exclus par defaut) et leurs
# suites prefixees IPv6 (selectionnables par filtre) — d'ou les deux options.
check-ipv6: tests/runtests
	./tests/runtests --gtest_filter='IPv6*' --gtest_also_run_disabled_tests

.PHONY: tests check check-ipv6

############################################
# Dependances generees automatiquement (headers -> objets)
############################################
-include $(wildcard $(BUILD)/*.d)
