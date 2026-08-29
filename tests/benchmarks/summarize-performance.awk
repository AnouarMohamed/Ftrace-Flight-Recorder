BEGIN {
	FS = "\t"
	order[1] = "baseline"
	order[2] = "4k"
	order[3] = "8k"
	order[4] = "16k"
	order[5] = "64k"
}

NR == 1 { next }

{
	candidate = $1
	runs[candidate]++
	output[candidate] += $4
	user_ticks[candidate] += $11
	system_ticks[candidate] += $12
	read_calls[candidate] += $13
	read_chars[candidate] += $15
	if ($17 > max_hwm[candidate])
		max_hwm[candidate] = $17
	loss[candidate] += $7 + $8 + $9 + $10
}

END {
	print "| Candidate | Runs | Captured | CPU s/GiB | System s/GiB | Reads/MiB | Mean bytes/read | Max VmHWM | Integrity loss |"
	print "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
	for (i = 1; i <= 5; i++) {
		candidate = order[i]
		mib = output[candidate] / 1048576
		total_seconds = (user_ticks[candidate] + \
		    system_ticks[candidate]) / ticks_per_second
		system_seconds = system_ticks[candidate] / ticks_per_second
		printf "| %s | %d | %.2f MiB | %.3f | %.3f | %.1f | %.1f | %d KiB | %d |\n", \
		    candidate, runs[candidate], mib,
		    total_seconds / (mib / 1024),
		    system_seconds / (mib / 1024),
		    read_calls[candidate] / mib,
		    read_chars[candidate] / read_calls[candidate],
		    max_hwm[candidate], loss[candidate]
	}
}
