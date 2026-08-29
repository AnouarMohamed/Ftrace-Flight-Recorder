#!/bin/sh
set -eu

binary=${1:-.build/benchmark_harvest}
rounds=${FDR_BENCH_ROUNDS:-5}
bytes=${FDR_BENCH_BYTES:-67108864}

case $rounds in
*[!0-9]*|'')
	echo "FDR_BENCH_ROUNDS must be a positive integer" >&2
	exit 2
	;;
esac
if [ "$rounds" -eq 0 ]; then
	echo "FDR_BENCH_ROUNDS must be a positive integer" >&2
	exit 2
fi

echo "benchmark=collector-copy rounds=$rounds bytes=$bytes"
echo "kernel=$(uname -r) architecture=$(uname -m)"
round=1
while [ "$round" -le "$rounds" ]; do
	printf 'round=%s ' "$round"
	FDR_BENCH_BYTES=$bytes "$binary"
	round=$((round + 1))
done
