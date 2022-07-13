#!/usr/bin/bash
#
# This script DPU utility functions
#

# Global settings (not used yet)
DPU_PORT1=9999
DPU_PORT2=11112
DPU_CONFIGFILE_PREFIX="/tmp/bws-dpu-cgf"

# Create the config file for the DPU offload engine
#
# Param dpulist Comma seperated list of hosts with DPUs
#
# Uses SLURM_JOB_PARTITION SLURM_NODELIST
# Exports OFFLOAD_CONFIG_FILE_PATH path to the DPU daemon config file
# Exports DPU_OFFLOAD_LIST_DPUS Comma separated list of hosts with DPUs
#
function dpu_create_configfile
{

	local dpulist="$1"
	if [ -z "$dpulist" ]; then
		echo "Usage: $0 <list of dpus>"
		exit 1
	else
		echo "DEBUG: dpulist=$dpulist"
	fi

	local domain=".hpcadvisorycouncil.com"
	if [ "thor" == "${SLURM_JOB_PARTITION}" ]; then
		local bf_ip_prefix="192.168.131.1"
		local hosts=$(scontrol show hostname ${SLURM_NODELIST} |grep -v 'bf' |sed -e "s/\$/${domain}/g")
		local bfs=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/\$/${domain}/g")
		local bf_ips=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/thorbf0/${bf_ip_prefix}/g")
		local cfg_names=$(paste <(echo "$hosts") <(echo "$bfs") -d ,)
		local cfg_names_ips=$(paste <(echo "$cfg_names") <(echo "$bf_ips") -d :)
		local dpu_cfg=$(sed -e "s/\$/:${DPU_PORT1}:${DPU_PORT2}\\n/g" <(echo "$cfg_names_ips") )
	elif [ "helios" == "${SLURM_JOB_PARTITION}" ]; then
		local bf_ip_prefix="192.168.129.1"
		local hosts=$(scontrol show hostname ${SLURM_NODELIST} |grep -v 'bf' |sed -e "s/\$/${domain}/g")
		local bfs=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/\$/${domain}/g")
		local bf_ips=$(scontrol show hostname ${SLURM_NODELIST} |grep 'bf' |sed -e "s/heliosbf0/${bf_ip_prefix}/g")
		local cfg_names=$(paste <(echo "$hosts") <(echo "$bfs") -d ,)
		local cfg_names_ips=$(paste <(echo "$cfg_names") <(echo "$bf_ips") -d :)
		local dpu_cfg=$(sed -e "s/\$/:${DPU_PORT1}:${DPU_PORT2}\\n/g" <(echo "$cfg_names_ips") )
	else
		echo "Unknown BF Testbed platform: ${SLURM_JOB_PARTITION}"
		exit 1
	fi

	# Create the config file
	rm -f /tmp/bws-dpu-cfg.*
	DPU_OFFLOAD_LIST_DPUS="$dpulist"
	OFFLOAD_CONFIG_FILE_PATH=$(mktemp /tmp/bws-dpu-cfg.XXXXXX)
	echo "# Format version: 1" > $OFFLOAD_CONFIG_FILE_PATH
	echo "# <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,..." >> $OFFLOAD_CONFIG_FILE_PATH
	for line in $dpu_cfg; do
		echo $line >> $OFFLOAD_CONFIG_FILE_PATH
	done

	export OFFLOAD_CONFIG_FILE_PATH
	export DPU_OFFLOAD_LIST_DPUS
}

# Distribute the DPU config file to al hosts
#
# Param configfile
#
# Uses SLURM_NODELIST
#
function dpu_send_configfile
{
	local configfile="$1"
	local hostlist=$(scontrol show hostname ${SLURM_NODELIST})
	for host in $hostlist; do
	        scp $OFFLOAD_CONFIG_FILE_PATH $host:$OFFLOAD_CONFIG_FILE_PATH
		[ $? != 0 ] && exit 1
	done
}

# Start the DPU daemons
#
# Param Comma separated list of hosts on which to start DPU daemons
#
function dpu_start_daemons
{
	local dpulist="$1"
	local daemondir="$2"

	if [ -z "$dpulist" -o -z "$daemondir" ]; then
		echo "Usage: $0 <list of dpus> <daemon exec dir>"
		exit 1
	else
		echo "DEBUG: dpulist=$dpulist"
		echo "DEBUG: daemondir=$daemondir"
	fi

	dpu_create_configfile "$dpulist"
	dpu_send_configfile "$OFFLOAD_CONFIG_FILE_PATH"

	# For each BF setup the environment and start it
	daemonexe="${daemondir}/bin/ucc_offload_dpu_daemon"
	daemonenv="UCX_NET_DEVICES=mlx5_0:1 \
		UCX_ZCOPY_THRESH=0 \
		UCX_TLS=rc_x \
		UCX_RC_TIMEOUT=inf \
		UCX_LOG_LEVEL=warn \
		UCX_LOG_PRINT_ENABLE=n \
		DPU_OFFLOAD_DBG_VERBOSE=1 \
		OFFLOAD_CONFIG_FILE_PATH=${OFFLOAD_CONFIG_FILE_PATH} \
		DPU_OFFLOAD_LIST_DPUS=${DPU_OFFLOAD_LIST_DPUS} \
		LD_LIBRARY_PATH=${DPU_SWHOME}/lib"

	echo "DEBUG: Config file: ${OFFLOAD_CONFIG_FILE_PATH}"
	for dpu in $(echo $dpulist |sed "s/,/ /g"); do
		daemonlog="$HOME/daemonlog-${SLURM_JOBID}-${dpu}.out"
	        ssh "$dpu" "${daemonenv} nohup $daemonexe &> $daemonlog &"
		echo "Daemon ($daemonexe) start status: $?"
	done

	local time=5s
	echo "Wait $time for daemon wireup to complete"
	sleep $time
}

# Stop the DPU daemons
#
# param Comma separated list of hosts with DPU
#
function dpu_stop_daemons {
	local dpulist=$1

	# Kill all the daemons
	for dpu in $(echo $dpulist |sed "s/,/ /g"); do
        	ssh $dpu "pkill -f ucc_offload_dpu_daemon; [ "\$?" == "0" ] && echo \"$dpu: Daemon stopped\""
	done
}
