#!/bin/bash

PROJET=mcumediaserver
VERSION="1.16.0"
#Repertoire d'installation des includes
DESTDIR_INC=/usr/include/
#Repertoire d'installation des librairies
if [ "`uname -m`" == "x86_64" ]
then
	DESTDIR_LIB=/usr/lib64
else
	DESTDIR_LIB=/usr/lib
fi

#RPepertoire d'installation des fichiers so
DESTDIR_MOD=$DESTDIR_LIB/asterisk/modules
#Repertoire d'installation du fichier mp4tool
DESTDIR_BIN=/usr/bin/
#Repertoire temporaire utiliser pour preparer les packages
TEMPDIR=/tmp

#Creation de l'environnement de packaging rpm
function create_rpm
{
    #Le spec est reserve a AlmaLinux 9 : dependances, scriptlets systemd et
    #chemins y sont ceux de la famille rpm. Le dire ici plutot que de laisser
    #rpmbuild manquer a l'appel, ou pire, produire un paquet inutilisable.
    detect_distro
    if [ "$DISTRO_FAMILY" != "rpm" ]
    then
        echo "La cible rpm demande une distribution de la famille RPM (AlmaLinux 9)."
        echo "Sur Debian/Ubuntu : ./install.ksh deb"
        exit 20
    fi

    #Cree l'environnement de creation de package
    #Creation des macros rpmbuild
    rm ~/.rpmmacros
    touch ~/.rpmmacros
	
    echo "%name" $PROJET >> ~/.rpmmacros
    echo "%version" $VERSION >> ~/.rpmmacros
    echo "%_topdir" $PWD"/rpmbuild" >> ~/.rpmmacros
    echo "%_tmppath %{_topdir}/TMP" >> ~/.rpmmacros
    echo "%_signature gpg" >> ~/.rpmmacros
    echo "%_gpg_name IVeSkey" >> ~/.rpmmacros
    echo "%_gpg_path" $PWD"/gnupg" >> ~/.rpmmacros
    echo "%vendor IVeS" >> ~/.rpmmacros
    if [[ -z $2 || $2 -ne nosign ]]
	then
		#Import de la clef gpg IVeS
		#svn export https://svn.ives.fr/svn-libs-dev/gnupg
		rm -rf gnupg
		git clone git@git.ives.fr:internal/gnupg.git
    fi
    mkdir -p rpmbuild
    mkdir -p rpmbuild/SOURCES
    mkdir -p rpmbuild/SPECS
    mkdir -p rpmbuild/BUILD
    mkdir -p rpmbuild/SRPMS
    mkdir -p rpmbuild/TMP
    mkdir -p rpmbuild/RPMS
    mkdir -p rpmbuild/RPMS/noarch
    mkdir -p rpmbuild/RPMS/i386
    mkdir -p rpmbuild/RPMS/i686
    mkdir -p rpmbuild/RPMS/i586
    mkdir -p rpmbuild/RPMS/x86_64
    #Recuperation de la description du package 
    cd ./rpmbuild/SPECS/
    cp ../../mcumediaserver.spec .
    cd ../../
    if [[ -z $2 || $2 -ne nosign ]]
	then
		rpmbuild -bb --sign $PWD/rpmbuild/SPECS/mcumediaserver.spec
	else
		rpmbuild -bb $PWD/rpmbuild/SPECS/mcumediaserver.spec
	fi
	if [ $? == 0 ]
	then
		echo "************************* fin du rpmbuild ****************************"
		#Recuperation du rpm
		mv -f $PWD/rpmbuild/RPMS/i386/*.rpm $PWD/.
		mv -f $PWD/rpmbuild/RPMS/i586/*.rpm $PWD/.
		mv -f $PWD/rpmbuild/RPMS/i686/*.rpm $PWD/.
		mv -f $PWD/rpmbuild/RPMS/x86_64/*.rpm $PWD/.
	clean
	else
	clean
	echo "*** error during build ***"
	exit 20
	fi
}

# Dependances du paquet .deb : celles que le binaire DECLARE lui-meme (DT_NEEDED),
# traduites en noms de paquets. Pas de liste ecrite a la main — elle vieillirait
# a chaque changement de version de ffmpeg — et pas la fermeture transitive
# d'ldd non plus : dpkg tire les dependances indirectes tout seul.
function deb_runtime_depends
{
	BINARY=$1

	for SONAME in $(objdump -p "$BINARY" | awk '/NEEDED/{print $2}')
	do
		SOPATH=$(ldd "$BINARY" | awk -v s="$SONAME" '$1==s{print $3}')
		[ -n "$SOPATH" ] && realpath -q "$SOPATH"
	done | sort -u | xargs -r dpkg -S 2>/dev/null \
	     | cut -d: -f1 | tr ',' '\n' | sed 's/ //g' | sort -u \
	     | paste -sd, - | sed 's/,/, /g'
}

# Paquet Debian/Ubuntu. Il installe les memes fichiers que le RPM, aux deux
# conventions Debian pres : les options vont dans /etc/default/mediaserver (que
# l'unite lit aussi, cf. mediaserver.service) et l'unite systemd dans
# /lib/systemd/system.
function create_deb
{
	detect_distro
	if [ "$DISTRO_FAMILY" != "deb" ]
	then
		echo "La cible deb demande une distribution Debian/Ubuntu (dpkg)."
		echo "Sur AlmaLinux 9 : ./install.ksh rpm"
		exit 20
	fi

	for TOOL in dpkg-deb dpkg-architecture objdump
	do
		command -v $TOOL > /dev/null 2>&1 || { echo "$TOOL absent : installer dpkg-dev et binutils"; exit 20; }
	done

	if [ ! -x bin/debug/mcu ]
	then
		echo "bin/debug/mcu absent : lancer d'abord ./install.ksh localcompile"
		exit 20
	fi

	ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)
	PKGROOT=$PWD/debbuild/${PROJET}_${VERSION}_${ARCH}

	echo "Construction du paquet ${PROJET}_${VERSION}_${ARCH}.deb"
	rm -rf "$PKGROOT"
	mkdir -p "$PKGROOT/DEBIAN"

	install -D -m 750 bin/debug/mcu            "$PKGROOT/opt/ives/bin/mediaserver"
	install -D -m 644 mediaserver.service      "$PKGROOT/lib/systemd/system/mediaserver.service"
	install -D -m 644 mediaserver.sysconfig    "$PKGROOT/etc/default/mediaserver"
	install -D -m 644 type-asian.xml           "$PKGROOT/etc/mediaserver/type-asian.xml"
	install -D -m 750 certcommunication.sh     "$PKGROOT/etc/mediaserver/certcommunication.sh"
	install -D -m 644 mcu.csr_conf             "$PKGROOT/etc/mediaserver/mcu.csr_conf"
	# Le binaire embarque le detecteur de voix de libfvad (BSD) : sa notice doit
	# accompagner la distribution binaire. Le sous-module libvad ne la porte pas.
	install -D -m 644 LICENSE.libfvad          "$PKGROOT/usr/share/doc/$PROJET/LICENSE.libfvad"

	DEPENDS=$(deb_runtime_depends bin/debug/mcu)
	INSTALLEDSIZE=$(du -ks "$PKGROOT" | cut -f1)

	cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: $PROJET
Version: $VERSION
Section: comm
Priority: optional
Architecture: $ARCH
Maintainer: IVeS <support@ives.fr>
Homepage: http://www.ives.fr
Installed-Size: $INSTALLEDSIZE
Depends: $DEPENDS
Description: IVeS mediaserver (MCU / serveur de media)
 Unite de conference multipoint et serveur de media : mixage audio, video,
 texte et partage de document, pilote en XML-RPC.
EOF

	# Equivalent de %config(noreplace) : dpkg n'ecrase pas un fichier modifie.
	cat > "$PKGROOT/DEBIAN/conffiles" <<EOF
/etc/default/mediaserver
/etc/mediaserver/mcu.csr_conf
EOF

	cat > "$PKGROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

if [ "$1" = "configure" ]
then
    systemctl daemon-reload || true
    systemctl enable mediaserver.service || true

    if [ ! -r /etc/ImageMagick-7/type.xml ]
    then
        echo "ImageMagick font are not configured. Participant name may not be displayed correctly"
    else
        cp /etc/mediaserver/type-asian.xml /etc/ImageMagick-7/
        if grep -q "type-asian.xml" /etc/ImageMagick-7/type.xml
        then
            echo "Asian font support has been correctly enabled"
        else
            echo "You need to change font configuration of ImageMagick for asian font support."
            echo 'Add the following line in type.xml: <include file="type-asian.xml" />'
        fi
    fi

    echo "Generating DTLS/OpenSSL certificate (ECDSA P-256) if needed"
    /etc/mediaserver/certcommunication.sh

    echo "Now (re)starting mediaserver"
    systemctl restart mediaserver.service || true
fi
EOF

	cat > "$PKGROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e

if [ "$1" = "remove" ]
then
    systemctl stop mediaserver.service || true
    systemctl disable mediaserver.service || true
fi
EOF

	cat > "$PKGROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e

if [ "$1" = "remove" ] || [ "$1" = "purge" ]
then
    systemctl daemon-reload || true
fi
EOF

	chmod 755 "$PKGROOT/DEBIAN/postinst" "$PKGROOT/DEBIAN/prerm" "$PKGROOT/DEBIAN/postrm"

	# --root-owner-group : les fichiers appartiennent a root dans le paquet sans
	# qu'il faille construire en root.
	dpkg-deb --root-owner-group --build "$PKGROOT" "$PWD/${PROJET}_${VERSION}_${ARCH}.deb"
	if [ $? != 0 ]
	then
		echo "*** error during build ***"
		exit 20
	fi

	rm -rf "$PWD/debbuild"
	echo "Paquet produit : ${PROJET}_${VERSION}_${ARCH}.deb"
}

function clean
{
	BASESRCDIR=$PWD
	MEDKITDIR=$BASESRCDIR/third_party/fontventa/libmedikit
	VADDIR=$BASESRCDIR/third_party/libvad/sources

  	# On efface les liens ainsi que le package precedemment cr.
  	echo Effacement des fichiers et liens gnupg rpmbuild ${PROJET}.rpm ${TEMPDIR}/${PROJET}
  	rm -rf gnupg rpmbuild ${TEMPDIR}/${PROJET}

	# Nettoyage du binaire et des objets du mediaserver.
	cd mcu
	make clean
	cd "$BASESRCDIR"

	# Nettoyage des objets et archives des sous-modules (libmedkit +
	# libvad), pour qu'un "clean" reparte reellement d'un arbre vierge. On garde
	# les memes options que la construction (compile_libmedkit
	# / compile_libvad).
	if [ -f "$MEDKITDIR/Makefile" ]
	then
		echo "Nettoyage libmedkit (in-tree) : objets + libmedkit.a"
		make -C "$MEDKITDIR" clean ASTERISK=no
		# Balayage defensif : la cible clean ne retire que les .o de la liste OBJS
		# courante ; on supprime aussi les .o residuels d'anciennes listes/builds
		# (aucun .o n'est suivi par git dans le sous-module).
		find "$MEDKITDIR" -name '*.o' -delete
		rm -f "$MEDKITDIR/libmedkit.a"
	fi
	if [ -f "$VADDIR/Makefile" ]
	then
		echo "Nettoyage libvad (in-tree) : objets + libfvad.a"
		# Sa cible clean efface les .o des trois repertoires et tous les .a.
		make -C "$VADDIR" clean
	fi
}

# Famille de la distribution : elle decide du gestionnaire de paquets, des noms
# de paquets et de la facon d'interroger l'installe. Deux familles supportees,
# AlmaLinux 9 (la cible de production) et Debian/Ubuntu.
function detect_distro
{
	if command -v rpm > /dev/null 2>&1
	then
		DISTRO_FAMILY=rpm
	elif command -v dpkg-query > /dev/null 2>&1
	then
		DISTRO_FAMILY=deb
	else
		echo "Distribution non reconnue : ni rpm ni dpkg."
		exit 20
	fi
}

# Paquets de developpement requis, par famille. Les deux listes decrivent les
# MEMES bibliotheques : ffmpeg, libsrtp2, xmlrpc-c, usrsctp, Magick++, libtool.
#
# gsm n'y figure plus : le codec GSM passe par ffmpeg (libmedikit/gsm/ enveloppe
# FfAudioCodec), plus aucun appel direct a l'API gsm.
#
# webrtc-audio-processing n'y figure plus non plus : la VAD passe par le
# sous-module libvad (fvad), bati in-tree. C'en etait le seul consommateur.
RPM_PREREQ="ffmpeg-devel libsrtp-devel xmlrpc-c-devel usrsctp-devel ImageMagick-c++-devel libtool"
DEB_PREREQ="libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev libavdevice-dev libsrtp2-dev libxmlrpc-core-c3-dev libxmlrpc-c++9-dev libusrsctp-dev libmagick++-dev libssl-dev libxml2-dev zlib1g-dev libbz2-dev libtool autoconf automake pkg-config"

function check_prereq
{
	detect_distro
	echo "checking if dependencies are installed ($DISTRO_FAMILY)"

	if [ "$DISTRO_FAMILY" == "rpm" ]
	then
		PKGLIST="$RPM_PREREQ"
	else
		PKGLIST="$DEB_PREREQ"
	fi

	MISSING=""
	for PKG in $PKGLIST
	do
		if [ "$DISTRO_FAMILY" == "rpm" ]
		then
			rpm -q "$PKG" > /dev/null 2>&1 || MISSING="$MISSING $PKG"
		else
			dpkg-query -W -f='${Status}' "$PKG" 2>/dev/null | grep -q "install ok installed" || MISSING="$MISSING $PKG"
		fi
	done

	if [ -n "$MISSING" ]
	then
		echo "Paquets manquants :$MISSING"
		echo "Les installer : ./install.ksh prereq"
		exit 20
	fi
}

function compile_mp4v2
{
	BASESRCDIR=$1

	if [ -f staticdeps/lib/libmp4v2.a ]
	then
		return
	fi

	echo "compilation libmp4v2"
	cd $HOME
	if [ ! -r mp4v2 ]
	then
		git clone https://github.com/InteractiviteVideoEtSystemes/mp4v2.git
	fi
	cd mp4v2
	# Les autotools versionnes dans mp4v2 datent d'automake 1.13 : ailleurs que
	# sur la machine qui les a produits, config.status regenere un script libtool
	# tronque, et le lien de la bibliotheque ne produit alors rien, sans erreur.
	# On les regenere avec ceux de la distribution.
	autoreconf -fi
	./configure --prefix=$BASESRCDIR/staticdeps --exec-prefix=$BASESRCDIR/staticdeps --enable-shared=no
	make clean
	# mp4v2 est du C++ d'avant C++11 : GCC >= 14 refuse ses conversions
	# retrecissantes en liste et sa comparaison pointeur/entier de rtphint.cpp.
	make CXXFLAGS="-g -O2 -Wno-narrowing -fpermissive"
	make install
	cd $BASESRCDIR
}

function local_compile
{
	# compiler localement
	check_prereq

	BASESRCDIR=$PWD

	compile_mp4v2 "$BASESRCDIR"

	# speex : plus de build statique. Le codec Speex est fourni par libmedikit
	# au-dessus de ffmpeg (AV_CODEC_ID_SPEEX, cf. libmedikit/speex/speexcodec.cpp)
	# et la ligne de lien el9 par defaut ne reference plus -lspeex.

	# libspeexdsp : plus utilisee. Le reechantillonnage audio (ex-AudioTransrater)
	# passe desormais par libswresample (ffmpeg), deja lie via -lswresample.

	# xmlrpc-c : plus construit depuis les sources. On s'appuie desormais sur le
	# paquet systeme xmlrpc-c-devel (AlmaLinux 9, depot crb) : memes en-tetes,
	# meme backend libxml2, lie dynamiquement (voir mcu/Makefile LDXMLFLAGS).

	# g722_1 / SIREN : plus construit. Le codec G.722.1 a ete retire du
	# mediaserver et de libmedikit (plus aucune reference a -lg722_1).

	cd $BASESRCDIR

	# Sous-modules (libmedkit = codecs, libvad = VAD) : on les
	# initialise au besoin puis on construit leurs archives in-tree, pour qu'un
	# seul "install.ksh localcompile" suffise a produire le binaire.
	if [ ! -f third_party/fontventa/libmedikit/medkit/media.h ] || [ ! -f third_party/libvad/sources/Makefile ]
	then
		echo "initialisation des sous-modules (libmedikit, libvad)"
		git submodule update --init --recursive
	fi
	compile_libmedkit
	compile_libvad

	cd $BASESRCDIR

	mkdir -p bin/debug
	cd mcu
	make mcu
}

function compile_rabbitmq
{
	MEDIASERVERPATH=$PWD
	rpm -q cmake > /dev/null
	if [ $? != 0 ]
	then
		echo "Installer cmake (sudo yum install cmake)"
		exit 20
	fi

	if [ ! -r staticdeps/lib/librabbitmq.a ]
	then
		echo "Compilation RABBITMQ-C"
		cd $HOME
		svn export http://svn.ives.fr/svn-libs-dev/rabbitmq-c/tags/0.3.0 rabbitmq-c
		cd rabbitmq-c
		mkdir build
		cd build
		cmake -DCMAKE_INSTALL_PREFIX=$MEDIASERVERPATH/staticdeps -DBUILD_STATIC_LIBS=1 -DBUILD_SHARED_LIBS=0 ..
		make
		make install
	fi

	if [ ! -r staticdeps/lib/libamqpcpp.a ]
	then
		echo "Compilation AMPQCPP"
		cd $HOME
		svn export http://svn.ives.fr/svn-libs-dev/ampqcpp/trunk ampqcpp
		cd ampqcpp
		make clean
		make INSTALLPREFIX=$MEDIASERVERPATH/staticdeps lib
		cp libamqpcpp.a $MEDIASERVERPATH/staticdeps/lib
		cp include/AMQPcpp.h $MEDIASERVERPATH/staticdeps/include
		
	fi
	cd $MEDIASERVERPATH
}

function compile_libmedkit
{
	# Construit libmedkit.a DANS l'arbre du sous-module (cible 'all', pas
	# d'install dans /opt/ives). Le mediaserver s'y lie directement via
	# MEDKITDIR/USEMEDKIT dans mcu/Makefile. Voir almalinux9_port_plan.md.
	MEDIASERVERPATH=$PWD
	MEDKITDIR=$MEDIASERVERPATH/third_party/fontventa/libmedikit
	if [ ! -d "$MEDKITDIR" ]
	then
		echo "Sous-module libmedikit absent. Lancer : git submodule update --init"
		exit 20
	fi
	echo "Compilation libmedkit (in-tree)"
	# Plus de surcharge d'INCLUDE ici : les chemins d'en-tetes sont TOUS dans le
	# Makefile du sous-module. ffmpeg y vient de pkg-config, staticdeps/include
	# du defaut relatif (equivalent au chemin absolu qu'on injectait, make
	# tournant en -C), et FFMPEGINC/MP4V2INC y restent utilisables comme
	# variables d'environnement.
	#
	# Cet ecrasement etait actif ET nuisible : il masquait un `-I` sans argument
	# du Makefile (CUSTOM_ASTPATH vide en build non-Asterisk), qui faisait avaler
	# a gcc l'option suivante comme repertoire d'include. Un `make` direct dans le
	# sous-module compilait donc SANS -DLOG_, contrairement au build officiel :
	# deux verites pour un meme arbre, et la mauvaise etait la plus accessible.
	#
	# ASTERISK=no : on exclut les objets couples a Asterisk (transcoder, mp4format,
	# framebuffer, frameutils, astlog), inutilisables hors module Asterisk et qui
	# exigeraient asterisk-devel. Le mediaserver n'est pas un module Asterisk.
	# On laisse le Makefile choisir la liste OBJS (source unique de verite : elle
	# inclut mp4reader.o/mp4writer.o dont depend le mediaserver via mp4streamer/
	# mp4recorder). Ne plus surcharger OBJS ici pour eviter la desynchronisation.
	make -C "$MEDKITDIR" all ASTERISK=no
	cd $MEDIASERVERPATH
}

function compile_libvad
{
	# Construit l'archive fvad DANS l'arbre du sous-module libvad, SANS y ecrire
	# le moindre fichier : LIB_SRC, ST_LIB et CFLAGS sont surcharges en ligne de
	# commande. Le sous-module pointe sur un depot tiers qu'IVeS ne forke pas,
	# donc tout ce qui nous est propre doit tenir ici.
	#
	# LIB_SRC='$(FVAD_SRC)' n'est pas une liste ecrite a la main : c'est la
	# variable du Makefile amont, reevaluee chez lui. Elle designe les 12 sources
	# fvad et rien d'autre. On ecarte ainsi sivr-vad.c, pour deux raisons :
	#  - il ne compile pas hors FreeSWITCH. Il emploie int16_t, malloc, free,
	#    memset, strcmp et abs sans leurs en-tetes ; c'est switch.h qui les
	#    fournissait ;
	#  - sa machine a etats (start/stop talking, hysteresis) ne rend pas le 0/1
	#    par trame que pipeaudiooutput cumule. Le mediaserver appelle fvad_* en
	#    direct, comme le fait sivr-vad lui-meme.
	# ST_LIB=libfvad.a nomme l'archive pour ce qu'elle contient : la libsivrvad.a
	# par defaut ne porterait aucun objet sivr.
	MEDIASERVERPATH=$PWD
	VADDIR=$MEDIASERVERPATH/third_party/libvad/sources
	if [ ! -f "$VADDIR/Makefile" ]
	then
		echo "Sous-module libvad absent. Lancer : git submodule update --init"
		exit 20
	fi
	if [ ! -f "$VADDIR/libfvad.a" ]
	then
		echo "Compilation libvad (in-tree, sous-ensemble fvad)"
		make -C "$VADDIR" \
			CFLAGS="-g -O2 -fPIC -I./sources -I./sources/signal_processing" \
			LIB_SRC='$(FVAD_SRC)' \
			ST_LIB=libfvad.a
	fi
	cd $MEDIASERVERPATH
}

function compile_protobuf
{
	MEDIASERVERPATH=$PWD

	if [ ! -r staticdeps/lib/libprotobuf.a ]
	then
		echo "Compilation PROTOBUF"
		cd $HOME
		svn export http://svn.ives.fr/svn-libs-dev/protobuf/tags/2.5.0 protobuf
		cd protobuf
		./configure --prefix=$MEDIASERVERPATH/staticdeps --exec-prefix=$MEDIASERVERPATH/staticdeps --enable-shared=no
		make
		make install
		cd $BASESRCDIR
	fi
	cd $MEDIASERVERPATH
}


case $1 in
  	"clean")
  		echo "Nettoyage des liens et du package crees par la cible dev"
  		clean ;;
  	"rpm")
		echo "Creation du rpm"
		create_rpm "$@";;

	"deb")
		create_deb;;
	"export")
        echo "{" >> build.properties
        echo "'VERSION': '$VERSION'," >> build.properties
        echo "'PROJET':'$PROJET'," >> build.properties
        echo "'DESTDIR':'$DESTDIR'" >> build.properties
        echo "}" >> build.properties
       ;;
	"localcompile")
		local_compile;;

    "rabbitmq")
		compile_rabbitmq;;

	"protobuf")
		compile_protobuf;;

	"libmedkit")
		compile_libmedkit;;


	"libvad")
		compile_libvad;;

	"upload")
		upload_rpm ;;
	"prereq")
		detect_distro
		if [ "$DISTRO_FAMILY" == "rpm" ]
		then
			sudo yum install -y $RPM_PREREQ
		else
			sudo apt-get install -y $DEB_PREREQ
		fi ;;
  	*)
  		echo "usage: install.ksh [options]" 
  		echo "options :"
  		echo "  rpm				Generation d'un package rpm (AlmaLinux 9)"
		echo "  deb             Generation d'un package deb (Debian/Ubuntu)"
		echo "  localcompile	Compilation du logiciel sans creation de paquet rpm"
		echo "  rabbitmq        Compilation des libs RABBITMQ (projet moteli)"
		echo "  libmedkit       Compilation de libmedkit.a (sous-module, in-tree)"
		echo "  libvad          Compilation de libfvad.a (sous-module, in-tree)"
		echo "  upload          TODO: envoi les paquets RPM dans le repo"
  		echo "  clean			Nettoie les fichiers crees par ce script (liens, rpm) + les objets/archives de mcu et des sous-modules (libmedkit, libvad)";;
esac
