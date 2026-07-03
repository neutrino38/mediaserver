#!/bin/bash

PROJET=mcumediaserver
VERSION="1.9.0"
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
    rm -rf $HOME/xmlrpc-c
    rm -f staticdeps/lib/libxmlrpc*.a
	
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

function clean
{
  	# On efface les liens ainsi que le package precedemment cr.
  	echo Effacement des fichiers et liens gnupg rpmbuild ${PROJET}.rpm ${TEMPDIR}/${PROJET}
  	rm -rf gnupg rpmbuild ${TEMPDIR}/${PROJET}
	cd mcu 
	make -f Makefile.rpm clean
	cd -
}

function local_compile
{
	# compiler localement
	echo checking if dependencies are installed
	rpm -q gsm-devel
    	if [ $? != 0 ]
	then
		echo "installer gsm-devel"
		exit 20
	fi

	rpm -q ffmpeg-devel
    if [ $? != 0 ]
	then
		echo "installer ffmpeg-free-devel depuis RPMFUSION free et non free"
		exit 20
	fi

	rpm -q libtool
    if [ $? != 0 ]
	then
		echo "installer libtool"
		exit 20
	fi

	rpm -q webrtc-audio-processing-devel
    if [ $? != 0 ]
	then
		echo "installer webrtc-audio-processing-devel"
		exit 20
	fi

	rpm -q libsrtp2-devel
    if [ $? != 0 ]
	then
		echo "installer libsrtp2-devel"
		exit 20
	fi


	# compiler openssl en statique
	BASESRCDIR=$PWD

	# compiler mp4v2 en static 
	if [ ! -f staticdeps/lib/libmp4v2.a ]
	then
		echo "compilation libmp4v2"
		cd $HOME
		if [ ! -r mp4v2 ]
		then
			git clone https://github.com/InteractiviteVideoEtSystemes/mp4v2.git
		fi
		cd mp4v2
		./configure --prefix=$BASESRCDIR/staticdeps --exec-prefix=$BASESRCDIR/staticdeps --enable-shared=no
		make clean
		make
		make install
		cd $BASESRCDIR
	fi
	
	# compiler speex en statique
	BASESRCDIR=$PWD
	if [ ! -f staticdeps/lib/libspeex.a ]
	then
		echo "on doit compiler SPEEX 1.2rc1"
		if [ ! -r $HOME/speex-src ]
		then
			#svn export http://svn.ives.fr/svn-libs-dev/asterisk/libsmedia/speex/tags/1.2rc1 $HOME/speex-src
			cd $HOME
			rm -f speex-1.2rc2.tar.gz speex-1.2rc2.tar
			wget http://downloads.xiph.org/releases/speex/speex-1.2rc2.tar.gz
			tar xzf speex-1.2rc2.tar.gz
			mv speex-1.2rc2 speex-src
			cd $HOME/speex-src
		fi
		cd $HOME/speex-src
			./configure --prefix=$BASESRCDIR/staticdeps --exec-prefix=$BASESRCDIR/staticdeps --enable-shared=no
		make
		make install
		cd $BASESRCDIR
	fi

	# libspeexdsp : plus utilisee. Le reechantillonnage audio (ex-AudioTransrater)
	# passe desormais par libswresample (ffmpeg), deja lie via -lswresample.

	# http://xmlrpc-c.svn.sourceforge.net/viewvc/xmlrpc-c/super_stable?view=tar
	if [ ! -f staticdeps/lib/libxmlrpc_abyss.a ]
	then
		echo "compilation XMLRPC-C"
		cd $HOME
		if [ ! -f xmlrpc-c ]
		then
			#svn export svn://svn.code.sf.net/p/xmlrpc-c/code/release_number/01.39.06 xmlrpc-c
			svn export svn://svn.code.sf.net/p/xmlrpc-c/code/stable xmlrpc-c
		fi
		cd xmlrpc-c
		./configure --disable-abyss-openssl --prefix=$BASESRCDIR/staticdeps --exec-prefix=$BASESRCDIR/staticdeps --enable-shared=no
		make
		make install
		cd $BASESRCDIR
	fi

	if [ ! -f staticdeps/lib/libg722_1.a ]
	then
		echo "compilation SIREN / G.722.1"
		cd $HOME
		if [ ! -r libg722_1 ]
		then
			#svn export http://svn.ives.fr/svn-libs-dev/libg722_1
			git clone https://github.com/neutrino38/libg722_1.git
		fi
		cd libg722_1
		./configure --prefix=$BASESRCDIR/staticdeps --exec-prefix=$BASESRCDIR/staticdeps --enable-shared=no
		make clean
		make
		make install
		cd $BASESRCDIR
	fi
	
	cd $BASESRCDIR

	# Sous-modules (libmedkit = codecs, libbfcp = BFCP) : on les initialise au
	# besoin puis on construit leurs archives in-tree, pour qu'un seul
	# "install.ksh localcompile" suffise a produire le binaire.
	if [ ! -f third_party/fontventa/libmedikit/medkit/media.h ] || [ ! -f third_party/libbfcp/Makefile ]
	then
		echo "initialisation des sous-modules (libmedikit, libbfcp)"
		git submodule update --init --recursive
	fi
	compile_libmedkit
	compile_libbfcp

	cd $BASESRCDIR

	mkdir -p bin/debug
	cd mcu
	make -f Makefile.rpm mcu
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
	# MEDKITDIR/USEMEDKIT dans mcu/Makefile.rpm. Voir almalinux9_port_plan.md.
	MEDIASERVERPATH=$PWD
	MEDKITDIR=$MEDIASERVERPATH/third_party/fontventa/libmedikit
	if [ ! -d "$MEDKITDIR" ]
	then
		echo "Sous-module libmedikit absent. Lancer : git submodule update --init"
		exit 20
	fi
	echo "Compilation libmedkit (in-tree)"
	# INCLUDE surcharge :
	#  - ffmpeg : en-tetes dans /usr/include/ffmpeg (override par FFMPEGINC) ;
	#  - mp4v2 : pas de paquet natif, en-tetes pris dans staticdeps/include
	#    (build source IVeS), override par MP4V2INC.
	# ASTERISK=no : on exclut les objets couples a Asterisk (transcoder, mp4format,
	# framebuffer, frameutils, astlog), inutilisables hors module Asterisk et qui
	# exigeraient asterisk-devel. Le mediaserver n'est pas un module Asterisk.
	# On laisse le Makefile choisir la liste OBJS (source unique de verite : elle
	# inclut mp4reader.o/mp4writer.o dont depend le mediaserver via mp4streamer/
	# mp4recorder). Ne plus surcharger OBJS ici pour eviter la desynchronisation.
	make -C "$MEDKITDIR" all \
		ASTERISK=no \
		INCLUDE="-I. ${FFMPEGINC:--I/usr/include/ffmpeg} ${MP4V2INC:--I$MEDIASERVERPATH/staticdeps/include}"
	cd $MEDIASERVERPATH
}

function compile_libbfcp
{
	# Construit libbfcp DANS l'arbre du sous-module (cible 'all', pas d'install
	# dans /opt/ives). Le mediaserver s'y lie directement via BFCPDIR dans
	# mcu/Makefile.rpm. On produit les deux variantes (dbg + rel) pour couvrir
	# les deux valeurs de DEBUG du build mcu.
	MEDIASERVERPATH=$PWD
	BFCPDIR=$MEDIASERVERPATH/third_party/libbfcp
	if [ ! -f "$BFCPDIR/Makefile" ]
	then
		echo "Sous-module libbfcp absent. Lancer : git submodule update --init"
		exit 20
	fi
	if [ ! -f "$BFCPDIR/lib/libbfcpdbg.a" ]
	then
		echo "Compilation libbfcp (in-tree, debug)"
		make -C "$BFCPDIR" all DEBUG=yes
	fi
	if [ ! -f "$BFCPDIR/lib/libbfcprel.a" ]
	then
		echo "Compilation libbfcp (in-tree, release)"
		make -C "$BFCPDIR" all DEBUG=no
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

	"libbfcp")
		compile_libbfcp;;

	"upload")
		upload_rpm ;;
	"prereq")
		sudo yum install -y gsm-devel ffmpeg-devel webrtc-audio-processing-devel libsrtp2-devel ;;
  	*)
  		echo "usage: install.ksh [options]" 
  		echo "options :"
  		echo "  rpm				Generation d'un package rpm"
		echo "  localcompile	Compilation du logiciel sans creation de paquet rpm"
		echo "  rabbitmq        Compilation des libs RABBITMQ (projet moteli)"
		echo "  libmedkit       Compilation de libmedkit.a (sous-module, in-tree)"
		echo "  libbfcp         Compilation de libbfcp (sous-module, in-tree)"
		echo "  upload          TODO: envoi les paquets RPM dans le repo"
  		echo "  clean			Nettoie tous les fichiers cree ce script, liens, tar.gz et rpm";;
esac
