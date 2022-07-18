# Get an allocation for hosts and their DPUs
#

# Print usage
function usage {
	echo "Usage: $0 <-v> <-p platform> <-n nnodes> <specific node>"
	exit 1
}

#
# Main
#

# Determine if the user requested a specific platform, otherwise set default
nhosts="1"
platform="thor"
verbose="0"
while getopts "n:p:v" o; do
	case ${o} in
		p)
			platoform=${OPTARG}
			;;
		n)
			nhosts=${OPTARG}
			;;
		v)
			echo "Dry run enabled"
			verbose=1
			;;
		*)
			usage
			;;
	esac
done
shift $((OPTIND-1))

# Determine if the user requested a specific first node
if [ -z "$1" ]; then
	begin=1
else
	begin="$1"
fi
echo "$platform count=$nhosts first=$begin"

# Hosts aren't named consistently, so we have to build the names differently
for n in $(seq $begin $((begin + nhosts - 1)) ); do
	if [ "thor" == "$platform" ]; then
		hostnum=$(seq -w $n 999 999)
		bfnum=$hostnum
		nodes="${platform}${hostnum},${platform}bf${bfnum}"
	elif [ "helios" == "$platform" ]; then
		hostnum=$(seq -w $n 999 999)
		bfnum=$hostnum
		nodes="${platform}${hostnum},${platform}bf${bfnum}"
	fi

	if [ -z "${nodelist}" ]; then
		nodelist="$nodes"
	else
		nodelist="${nodelist},${nodes}"
	fi
done

cmd="salloc -N $((nhosts*2)) -p ${platform} --nodelist ${nodelist} -t 120"
if [ "1" == "$verbose" ]; then
	echo "$cmd"
else
	$cmd
fi
