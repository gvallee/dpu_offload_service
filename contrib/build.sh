#!/bin/sh
# Environment setup stuff
module load poc-dpu-alltoallv
os=$(uname -r |grep -o -E '[a-z]+.' |head -n 1)
mtype=$(uname -m)
platform=${os}-${mtype}
export SWHOME=${HOME}/sw/poc-dpu-alltoallv/${platform}
export WORKSPACE=${HOME}/workspace/poc-dpu-alltoallv

# Identify what to build
if [ "x86_64" = "$mtype" ]; then
	builddir="build_x86"
elif [ "aarch64" = "$mtype" ]; then
	builddir="build_aarch64"
else
	echo "Error: Unknown machine type."
	exit 1
fi

if [ ! -z "${1}" ]; then
	echo "Requested targets for $SWHOME are $@"
	targets="$@"
else
	echo "Full build requested, deleting $SWHOME contents ..."
	sleep 4s
	rm -r $SWHOME/bin $SWHOME/lib $SWHOME/include $SWHOME/etc $SWHOME/share
	targets="ucx dpu_offload_service ompi ucc ompi+ucc"
fi

# Perform builds
for t in $targets; do
	echo "Building ${t} ..."
	case $t in
		ucx)
			dir=ucx
			cd ${dir} && ./autogen.sh >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		dpu_offload_service)
			dir=dpu_offload_service
			cd ${dir} && ./autogen.sh >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --with-ucx=$SWHOME --enable-debug >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		ompi)
			dir=ompi
			cd ${dir} && ./autogen.pl >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --with-ucx=$SWHOME --with-pmix=internal >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		ucc)
			dir=ucc-priv
			cd ${dir} && ./autogen.sh >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --enable-debug --with-ucx=$SWHOME --with-dpu-offload=$SWHOME --with-mpi=$SWHOME >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1

			# Build the daemon
			set -x
			UCC_SRC_DIR=${WORKSPACE}/ucc-priv
			DPU_DAEMON_BUILD=${UCC_SRC_DIR}/${builddir}
			DPU_DAEMON_SRC_DIR=${UCC_SRC_DIR}/src/components/tl/ucp/offload_dpu_daemon
			echo "Daemon dirs: <$UCC_SRC_DIR> <${DPU_DAEMON_BUILD}> <${DPU_DAEMON_SRC_DIR}>"
			cd ${DPU_DAEMON_BUILD}
			gcc ${DPU_DAEMON_SRC_DIR}/offload_dpu_daemon.c -O0 -g -I${DPU_DAEMON_SRC_DIR} -I${SWHOME}/include -L${SWHOME}/lib -ldpuoffloaddaemon -l ucp -l ucs -o ucc_offload_dpu_daemon
			[ $? != 0] && exit 1
			cp ucc_offload_dpu_daemon $SWHOME/bin
			cd ${WORKSPACE}
			;;
		ompi+ucc)
			dir=ompi
			cd ${dir} && ./autogen.pl >/dev/null
			[ $? != 0 ] && exit 1
			cd ..
			dir=${dir}/${builddir}
			mkdir -p ${dir} && cd ${dir}
			../configure --prefix=$SWHOME --with-ucx=$SWHOME --with-ucc=$SWHOME --with-pmix=internal >/dev/null
			[ $? != 0 ] && exit 1
			make -j $(nproc) && make install >/dev/null
			[ $? != 0 ] && exit 1
			cd ${WORKSPACE}
			;;
		*)
			echo "Unknown target ${t}!"
			echo "Available targets are 'ucx', 'dpu_offload_service', 'ompi', 'ucc', & 'ompi+ucc'"
			exit 2
			;;
	esac
done

echo "Completed building targets:${targets}"
