help()
{
    echo "Usage: record-host-metrics
               [ -d | --dur (duration in seconds to record each metric; default=30s) ] 
               [ -c | --cpu_util (=0/1, disable/enable recording cpu utilization) ) ] 
               [ -C | --cores (comma separated values of cpu cores to log utilization, eg., '0,4,8,12') ) ]  
               [ -P | --pcie (=0/1, disable/enable recording PCIe bandwidth) ] 
               [ -M | --membw (=0/1, disable/enable recording memory bandwidth) ] 
               [ -L | --llc (=0/1, disable/enable recording LLC misses) ]
               [ -I | --iio (=0/1, disable/enable recording IIO occupancy) ] 
               [ -p | --pfc (=0/1, disable/enable recording PFC pause triggers) ] 
               [ -i | --intf (interface name, over which to record PFC triggers) ] 
               [ -t | --type (=0/1, experiment type -- 0 for TCP, 1 for RDMA) ] 
               [ -h | --help  ]"
    exit 2
}

SHORT=d:,c:,C:,P:,M:,L:,I:,p:,O:,i:,h
LONG=dur:,cpu_util:,cores:,pcm_pcie:,pcm_mem:,llc:,pcm_iio:,pfc:,iio_occ:,intf:,help
OPTS=$(getopt -a -n record-host-metrics --options $SHORT --longoptions $LONG -- "$@")

VALID_ARGUMENTS=$# # Returns the count of arguments that are in short or long options

# if [ "$VALID_ARGUMENTS" -eq 0 ]; then
#   help
# fi

eval set -- "$OPTS"

#default values
dur=3
type=0
cpu_util=0
cores=20
pcm_pcie=0
pcm_mem=0
llc=1
pcm_iio=0
iio_occ=0
pfc=0
intf=rdma0

cur_dir=$PWD

while :
do
  case "$1" in
    -d | --dur )
      dur="$2"
      shift 2
      ;;
    -c | --cpu_util )
      cpu_util="$2"
      shift 2
      ;;
    -C | --cores )
      cores="$2"
      shift 2
      ;;
    -P | --pcie )
      pcie="$2"
      shift 2
      ;;
    -M | --membw )
      membw="$2"
      shift 2
      ;;
    -L | --llc )
      llc="$2"
      shift 2
      ;;
    -I | --iio )
      iio="$2"
      shift 2
      ;;
    -p | --pfc )
      pfc="$2"
      shift 2
      ;;
    -i | --intf )
      intf="$2"
      shift 2
      ;;
    -t | --type )
      type="$2"
      shift 2
      ;;
    -h | --help)
      help
      ;;
    --)
      shift;
      break
      ;;
    *)
      echo "Unexpected option: $1"
      help
      ;;
  esac
done

rm -f ./logs/*
mkdir -p logs #Directory to store collected logs
mkdir -p reports #Directory to store parsed metrics

echo "--------------------begin all measurement--------------------" > reports/report.rpt

function dump_iiobw() {
    modprobe msr
    sudo taskset -c 31 /usr/sbin/pcm-iio 1 -csv=logs/iio.csv &
}

function parse_iiobw() {
    echo "------------------iiobw performance------------------" >> reports/report.rpt
    echo "PCIe_rd(wr)_tput: bytes PCIe devices requested to read from(write to) mem through DMA" >> reports/report.rpt
    echo "MMIO_rd(wr)_tput: bytes cpu requested to read from(write to) PCIe devices through MMIO" >> reports/report.rpt
    echo "IOTLB_hits/misses: times of IOTLB hits/misses during DMA" >> reports/report.rpt
    #TACC_GPU13/15,rdma2: Socket1,IIO Stack 1 - PCIe0,Part0
    echo "PCIe_rd_tput: " $(cat logs/iio.csv | grep "Socket1,IIO Stack 1 - PCIe0,Part0" | awk -F ',' '{ sum += $5/1000000000.0; n++ } END { if (n > 0) printf "%0.3f", sum / n * 8 ; }') "Gbps" >> reports/report.rpt
    echo "PCIe_wr_tput: " $(cat logs/iio.csv | grep "Socket1,IIO Stack 1 - PCIe0,Part0" | awk -F ',' '{ sum += $4/1000000000.0; n++ } END { if (n > 0) printf "%0.3f", sum / n * 8 ; }') "Gbps" >> reports/report.rpt
    echo "MMIO_rd_tput: " $(cat logs/iio.csv | grep "Socket1,IIO Stack 1 - PCIe0,Part0" | awk -F ',' '{ sum += $6/1000000000.0; n++ } END { if (n > 0) printf "%0.3f", sum / n * 8 ; }') "Gbps" >> reports/report.rpt
    echo "MMIO_wr_tput: " $(cat logs/iio.csv | grep "Socket1,IIO Stack 1 - PCIe0,Part0" | awk -F ',' '{ sum += $7/1000000000.0; n++ } END { if (n > 0) printf "%0.3f", sum / n * 8 ; }') "Gbps" >> reports/report.rpt
    echo "IOTLB_hits: " $(cat logs/iio.csv | grep "Socket1,IIO Stack 1 - PCIe0,Part0" | awk -F ',' '{ sum += $8; n++ } END { if (n > 0) printf "%0.0f", sum / n; }') >> reports/report.rpt
    echo "IOTLB_misses: " $(cat logs/iio.csv | grep "Socket1,IIO Stack 1 - PCIe0,Part0" | awk -F ',' '{ sum += $9; n++ } END { if (n > 0) printf "%0.0f", sum / n; }') >> reports/report.rpt
}

function dump_pcie() {
    modprobe msr
    sudo taskset -c 31 /usr/sbin/pcm-pcie -e -csv=logs/pcie.csv &
}

function parse_pcie() {
  echo "------------------pcie performance------------------" >> reports/report.rpt
  /*TO DO*/
}

function dump_membw() {
    modprobe msr
    sudo taskset -c 31 /usr/sbin/pcm-memory 1 -columns=5
}

function parse_membw() {
    echo "--------------------mem performance--------------------" >> reports/report.rpt
    echo "metric       bandwidth(MB/s)" >> reports/report.rpt
    echo "Node0_rd_bw: " $(cat logs/membw.log | grep "NODE 0 Mem Read" | awk '{ sum += $8; n++ } END { if (n > 0) printf "%f\n", sum / n; }') >> reports/report.rpt
    echo "Node0_wr_bw: " $(cat logs/membw.log | grep "NODE 0 Mem Write" | awk '{ sum += $7; n++ } END { if (n > 0) printf "%f\n", sum / n; }') >> reports/report.rpt
    echo "Node0_total_bw: " $(cat logs/membw.log | grep "NODE 0 Memory" | awk '{ sum += $6; n++ } END { if (n > 0) printf "%f\n", sum / n; }') >> reports/report.rpt
    echo "Node1_rd_bw: " $(cat logs/membw.log | grep "NODE 1 Mem Read" | awk '{ sum += $16; n++ } END { if (n > 0) printf "%f\n", sum / n; }') >> reports/report.rpt
    echo "Node1_wr_bw: " $(cat logs/membw.log | grep "NODE 1 Mem Write" | awk '{ sum += $14; n++ } END { if (n > 0) printf "%f\n", sum / n; }') >> reports/report.rpt
    echo "Node1_total_bw: " $(cat logs/membw.log | grep "NODE 1 Memory" | awk '{ sum += $12; n++ } END { if (n > 0) printf "%f\n", sum / n; }') >> reports/report.rpt
}

function dump_llc() {
  dperf_pid_tmp=$(ps aux | grep dperf | grep -v grep | awk '{print $2}')
  dperf_pid=$(echo -e "$dperf_pid_tmp" | tr '\n' ',' | sed 's/,$//')
  if [ -z "$dperf_pid" ]; then
    echo "NO pipetune process detected"
    exit 1
  else
    echo "pipetune pid is $dperf_pid"
  fi
  # sudo taskset -c 31 perf stat -p $dperf_pid --no-big-num -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores -e LLC-loads -e LLC-load-misses -e LLC-stores -e LLC-store-misses -o logs/llc.log &
  sudo taskset -c 31 perf stat -p $dperf_pid --no-big-num -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores -e LLC-loads -e LLC-load-misses -e LLC-stores -e LLC-store-misses -o logs/llc.log &
  sleep $dur
  sudo kill -SIGINT $(ps aux | grep "perf" | awk '{print $2}')
}

function parse_llc() {
    echo "--------------------llc performance--------------------" >> reports/report.rpt
    sleep 2
    l1_loads=$(cat logs/llc.log | grep "L1-dcache-loads" | awk '{print $1}')
    l1_load_misses=$(cat logs/llc.log | grep "L1-dcache-load-misses" | awk '{print $1}')
    l1_dcache_stores=$(cat logs/llc.log | grep "L1-dcache-stores" | awk '{print $1}')
    l1_loads_miss_rate=$(echo $l1_load_misses $l1_loads | awk '{ printf "%0.2f\n" ,$1/$2}')

    llc_loads=$(cat logs/llc.log | grep "LLC-loads" | awk '{print $1}')
    llc_load_misses=$(cat logs/llc.log | grep "LLC-load-misses" | awk '{print $1}')
    llc_load_miss_rate=$(echo $llc_load_misses $llc_loads | awk '{ printf "%0.2f\n" ,$1/$2}')

    llc_stores=$(cat logs/llc.log | grep "LLC-stores" | awk '{print $1}')
    llc_store_misses=$(cat logs/llc.log | grep "LLC-store-misses" | awk '{print $1}')
    llc_store_miss_rate=$(echo $llc_store_misses $llc_stores | awk '{ printf "%0.2f\n" ,$1/$2}')

    echo "L1-dcache-loads:" $l1_loads >> reports/report.rpt
    echo "L1-dcache-load-misses:" $l1_load_misses >> reports/report.rpt
    echo "L1-dcache-stores:" $l1_dcache_stores >> reports/report.rpt
    echo "L1-dcache-load-misses-rate:" $l1_loads_miss_rate >> reports/report.rpt

    echo "LLC-loads:" $llc_loads >> reports/report.rpt
    echo "LLC-load-misses:" $llc_load_misses >> reports/report.rpt
    echo "LLC-load-misses-rate:" $llc_load_miss_rate >> reports/report.rpt

    echo "LLC-stores:" $llc_stores >> reports/report.rpt
    echo "LLC-store-misses:" $llc_store_misses >> reports/report.rpt
    echo "LLC-store-misses-rate:" $llc_store_miss_rate >> reports/report.rpt
}

function collect_pfc() {
    #assuming PFC is enabled for QoS 0
    sudo ethtool -S $intf | grep pause > logs/pause.before.log
    sleep $dur
    sudo ethtool -S $intf | grep pause > logs/pause.after.log

    pause_before=$(cat logs/pause.before.log | grep "tx_prio0_pause" | head -n1 | awk '{ printf $2 }')
    pause_duration_before=$(cat logs/pause.before.log | grep "tx_prio0_pause_duration" | awk '{ printf $2 }')
    pause_after=$(cat logs/pause.after.log | grep "tx_prio0_pause" | head -n1 | awk '{ printf $2 }')
    pause_duration_after=$(cat logs/pause.after.log | grep "tx_prio0_pause_duration" | awk '{ printf $2 }')

    echo "pauses_before: "$pause_before > logs/pause.log
    echo "pause_duration_before: "$pause_duration_before >> logs/pause.log
    echo "pauses_after: "$pause_after >> logs/pause.log
    echo "pause_duration_after: "$pause_duration_after >> logs/pause.log

    # echo $pause_before, $pause_after
    echo "print(($pause_after - $pause_before)/$dur)" | lua > reports/pause.rpt

    # echo $pause_duration_before, $pause_duration_after
    echo "print(($pause_duration_after - $pause_duration_before)/$dur)" | lua >> reports/pause.rpt
}


function compile_if_needed() {
    local source_file=$1
    local executable=$2

    # Check if the executable exists and if the source file is newer
    if [ ! -f "$executable" ] || [ "$source_file" -nt "$executable" ]; then
        echo "Compiling $source_file..."
        gcc -o "$executable" "$source_file"
        if [ $? -eq 0 ]; then
            echo "Compilation successful."
        else
            echo "Compilation failed."
        fi
    else
        echo "No need to recompile."
    fi
}

if [ "$cpu_util" = 1 ]
then
    echo "Collecting CPU utilization for cores $cores..."
    sar -P $cores 1 1000 > logs/cpu_util.log &
    sleep $dur
    sudo pkill -9 -f "sar"
    echo "--------------------cpu performance--------------------" >> reports/report.rpt
    python3 cpu_util.py logs/cpu_util.log >> reports/report.rpt
fi

if [ "$pcm_pcie" = 1 ]
then
    echo "Collecting pcm-pcie occupancy..."
    dump_pcie
    sleep $dur
    sudo pkill -9 -f "pcm"
    parse_pcie
fi


if [ "$pcm_iio" = 1 ]
then
    echo "Collecting pcm-iio bandwidth..."
    dump_iiobw
    sleep $dur
    sudo pkill -9 -f "pcm"
    parse_iiobw
fi

if [ "$iio_occ" = 1 ]
then
    echo "Collecting IIO occupancy..."
    echo "--------------------iio occupancy--------------------" >> reports/report.rpt
    echo "average occ = end_cumulative_occ - start_cumulative_occ / time(ns)" >> reports/report.rpt
    # gcc collect_iio_occ.c -o collect_iio_occ
    compile_if_needed collect_iio_occ.c collect_iio_occ
    for id in $(echo $cores | sed "s/,/ /g")
    do
      taskset -c $id ./collect_iio_occ $id &
      sleep 5
      sudo pkill -2 -f collect_iio_occ
      sleep 4
      mv iio.log logs/iio_occ_$id.log
      avg_occ=$(cat logs/iio_occ_$id.log | grep "average occ" | awk '{print $4}')
      echo "avgerage occ:" $avg_occ >> reports/report.rpt
      break;
    done
fi

if [ "$pcm_mem" = 1 ]
then
    echo "Collecting Memory bandwidth..."
    dump_membw > logs/membw.log &
    # sleep 30
    sleep $dur
    sudo pkill -9 -f "pcm"
    parse_membw
fi

if [ "$llc" = 1 ]
then
  echo "Collecting LLC references/misses..."
  dump_llc
  parse_llc
fi