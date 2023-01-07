#/bin/bash

COUNT=$1

if [[ -z "$COUNT" ]]; then
    echo "No arguments supplied. Number of processes is required."
    exit 1
fi

function cleanup() {
    rm -f /dev/shm/shared_memory*
}

cmake -S . -B build -DPROC_COUNT=$COUNT
cmake --build build

cleanup

# pids of processes
pids=()
for ((i=0;i<COUNT;i++)); do
    ./build/Proc $i &
    pids[$i]=$!
done

# echo ${pids[@]}

trap 'exit' SIGINT SIGTERM
trap 'cleanup; kill 0' EXIT

while :
do
    read -n1 -s num
    if [[ -v "pids[$num]" ]]; then
        if [[ "${pids[$num]}" == "-1" ]]; then
            echo Restart proc $num
            ./build/Proc $num &
            pids[$num]=$!
        else
            echo Kill proc $num
            kill ${pids[$num]}
            pids[$num]="-1"
        fi
    else
        echo "Wrong index"
    fi
done
