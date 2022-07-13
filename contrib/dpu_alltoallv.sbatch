#!/usr/bin/bash
#
# This script runs a set of benchmarks using DPUs
#
#SBATCH --job-name=dpu-alltoallv
##SBATCH --output=$HOME/jobs/%x-%j.out
#SBATCH --nodes=2
#SBATCH --time=00:60:00
#SBATCH --exclusive
#SBATCH -d singleton

### Platform Settings
nodes=$((SLURM_NNODES/2))
slots=${SLURM_CPUS_ON_NODE}
sockets=0
hostib="mlx5_100:100"
case "${SLURM_JOB_PARTITION}" in
	thor)
		sockets=1
		hostib=mlx5_2:1
		;;
	helios)
		sockets=1
		hostib=mlx5_4:1
		;;
	*)
		echo "This script has not been tested on partition: ${SLURM_JOB_PARTITION}"
		exit 1
		;;
esac

### Job Settings
JOB_PPN=$slots
JOB_NPROCS=$((nodes*slots))
JOB_ITER=1
JOB_NPROCS=$((2))

### Platform/DPU software locations
export PRTE_MCA_plm=ssh
export SWHOME="${HOME}/sw/poc-dpu-alltoallv/el8-x86_64"
export DPU_SWHOME="${HOME}/sw/poc-dpu-alltoallv/bluefield-aarch64"
echo "SWHOME: $SWHOME"
echo "DPU_SWHOME: ${DPU_SWHOME}"

# Create a list of just the hosts and a list of just the DPUs
hostlist=$(scontrol show hostname ${SLURM_NODELIST} |grep -v 'bf' |sed -e "s/\$/:${slots}/g" |paste -d , -s)
bflist=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/\$//g" |paste -d , -s)
echo "Hostname list: $hostlist"
echo "Bluefield list: $bflist"

# Import the DPU utility functions
if [ -f ~/srun/dpu_utils.sh ]; then
	source ~/srun/dpu_utils.sh
else
	echo "Unable to import: ~/srun/dpu_utils.sh"
	exit 1
fi

dpu_stop_daemons "$bflist"
dpu_start_daemons "$bflist" "${DPU_SWHOME}"

# Run the dpu assisted MPI command
export LD_LIBRARY_PATH=$SWHOME/lib
echo "Offload Config file: $OFFLOAD_CONFIG_FILE_PATH:"
cat $OFFLOAD_CONFIG_FILE_PATH
which $SWHOME/bin/mpirun
unset SLURM_JOBID

# Run collective through an mpi test program (set coll_ucc_enable=1 and coll_ucc_priority=100)
time $SWHOME/bin/mpirun \
    --np ${JOB_NPROCS} \
    --map-by ppr:${JOB_PPN}:node:oversubscribe \
    -H ${hostlist} \
    --bind-to core \
    --rank-by core \
    --mca pml ucx \
        -x UCX_NET_DEVICES=$hostib \
        -x UCX_TLS=rc_x \
        -x UCX_LOG_LEVEL=warn \
        -x UCX_LOG_PRINT_ENABLE=n \
    --mca coll_hcoll_enable 0 \
    --mca coll_ucc_enable 1 \
    --mca coll_ucc_priority 100 \
        -x UCC_CL_BASIC_TLS=ucp \
        -x UCC_LOG_LEVEL=debug \
        -x OFFLOAD_CONFIG_FILE_PATH=$OFFLOAD_CONFIG_FILE_PATH \
        -x DPU_OFFLOAD_DBG_VERBOSE=1 \
    stdbuf -e0 -o0 \
    $HOME/workspace/poc-dpu-alltoallv/mpi_tests/alltoallv/simple_alltoallv 2>&1 | tee alltoallv-log


dpu_stop_daemons "$bflist"
