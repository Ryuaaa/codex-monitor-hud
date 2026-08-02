#!/bin/zsh

# Lightweight, privacy-preserving macOS + Codex resource sampler.
# It records numeric aggregates only. Process arguments and user content are
# never written to disk.

set -u

script_dir="${0:A:h}"
data_dir="$script_dir/data"
lock_dir="$script_dir/.collect.lock"

/bin/mkdir -p "$data_dir"

if ! /bin/mkdir "$lock_dir" 2>/dev/null; then
  exit 0
fi
trap '/bin/rmdir "$lock_dir" 2>/dev/null || true' EXIT INT TERM

timestamp=$(/bin/date '+%Y-%m-%dT%H:%M:%S%z')
epoch=$(/bin/date '+%s')
day=$(/bin/date '+%Y-%m-%d')
data_file="$data_dir/$day.csv"

pressure_output=$(/usr/bin/memory_pressure 2>/dev/null || true)
total_bytes=$(/usr/bin/printf '%s\n' "$pressure_output" | /usr/bin/awk '/The system has/{print $4; exit}')
available_pct=$(/usr/bin/printf '%s\n' "$pressure_output" | /usr/bin/awk -F': ' '/System-wide memory free percentage/{gsub(/%/,"",$2); print $2; exit}')
page_size=$(/usr/bin/printf '%s\n' "$pressure_output" | /usr/bin/awk '/page size of/{gsub(/[^0-9]/,"",$NF); print $NF; exit}')
compressor_pages=$(/usr/bin/printf '%s\n' "$pressure_output" | /usr/bin/awk -F': ' '/Pages used by compressor/{print $2; exit}')

if [[ -z "$total_bytes" || -z "$available_pct" ]]; then
  exit 1
fi

available_gib=$(/usr/bin/awk -v total="$total_bytes" -v pct="$available_pct" 'BEGIN{printf "%.2f", total*pct/100/1073741824}')
compressed_gib=$(/usr/bin/awk -v pages="${compressor_pages:-0}" -v size="${page_size:-16384}" 'BEGIN{printf "%.2f", pages*size/1073741824}')

swap_mib=$(/usr/sbin/sysctl vm.swapusage 2>/dev/null | /usr/bin/awk '{for(i=1;i<=NF;i++) if($i=="used"){v=$(i+2); unit=substr(v,length(v),1); sub(/[MGT]$/,"",v); if(unit=="G")v*=1024; if(unit=="T")v*=1048576; printf "%.2f",v; exit}}')

load_values=$(/usr/sbin/sysctl -n vm.loadavg 2>/dev/null | /usr/bin/awk '{printf "%s,%s,%s",$2,$3,$4}')
if [[ -z "$load_values" ]]; then
  load_values=",,"
fi

disk_free_gib=$(/bin/df -k /System/Volumes/Data 2>/dev/null | /usr/bin/awk 'NR==2{printf "%.2f",$4/1048576; exit}')
if [[ -z "$disk_free_gib" ]]; then
  disk_free_gib=$(/bin/df -k / 2>/dev/null | /usr/bin/awk 'NR==2{printf "%.2f",$4/1048576; exit}')
fi

# Aggregate the complete ChatGPT/Codex process tree. The ps command deliberately
# uses `comm` (executable path only), never `command`/arguments.
process_metrics=$(/bin/ps -axo pid=,ppid=,%cpu=,rss=,etime=,comm= 2>/dev/null | /usr/bin/awk -v codex_home="$HOME/.codex/" '
  function seconds(value, parts, n, days) {
    days=0
    if (value ~ /-/) {
      split(value, parts, "-")
      days=parts[1]
      value=parts[2]
    }
    n=split(value, parts, ":")
    if (n==3) return days*86400 + parts[1]*3600 + parts[2]*60 + parts[3]
    if (n==2) return days*86400 + parts[1]*60 + parts[2]
    return days*86400 + parts[1]
  }
  {
    pid[NR]=$1
    ppid[NR]=$2
    cpu[NR]=$3
    rss[NR]=$4
    elapsed[NR]=$5
    cmd[NR]=$6
    for (i=7; i<=NF; i++) cmd[NR]=cmd[NR] " " $i
    index_by_pid[$1]=NR
    if (cmd[NR] ~ /^\/Applications\/ChatGPT\.app\// || index(cmd[NR], codex_home)==1) root[NR]=1
  }
  END {
    # Resolve ancestry once per process and cache the result. This avoids a
    # repeated whole-table scan when deeply nested tool processes are present.
    for (i=1; i<=NR; i++) {
      j=i
      depth=0
      while (j && !root[j] && !(j in resolved) && depth<NR) {
        chain[++depth]=j
        j=index_by_pid[ppid[j]]
      }
      yes=(j && (root[j] || resolved[j])) ? 1 : 0
      while (depth>0) {
        resolved[chain[depth]]=yes
        delete chain[depth]
        depth--
      }
      included[i]=(root[i] || resolved[i]) ? 1 : 0
    }
    for (i=1; i<=NR; i++) if (included[i]) {
      count++
      cpu_sum+=cpu[i]
      rss_sum+=rss[i]
      if (rss[i]>largest_rss) largest_rss=rss[i]
      if (cmd[i] ~ /Codex \(Renderer\)/) renderer_count++
      if (cmd[i] ~ /\/Resources\/codex$/ || cmd[i] ~ /\/Resources\/cua_node\// || cmd[i] ~ /codex-code-mode-host/ || cmd[i] ~ /computer-use/) helper_count++
      if (cmd[i] == "/Applications/ChatGPT.app/Contents/MacOS/ChatGPT") main_uptime=seconds(elapsed[i])
    }
    printf "%d,%.1f,%.2f,%.2f,%d,%d,%d", count+0, cpu_sum+0, rss_sum/1024, largest_rss/1024, renderer_count+0, helper_count+0, main_uptime+0
  }
')

if [[ -z "$process_metrics" ]]; then
  process_metrics="0,0.0,0.00,0.00,0,0,0"
fi

header='timestamp,epoch,system_available_pct,system_available_gib,compressed_gib,swap_used_mib,disk_free_gib,load_1m,load_5m,load_15m,codex_processes,codex_cpu_pct,codex_rss_mib,codex_largest_rss_mib,codex_renderers,codex_engine_tool_helpers,codex_main_uptime_seconds'
if [[ ! -f "$data_file" ]]; then
  /usr/bin/printf '%s\n' "$header" > "$data_file"
fi

/usr/bin/printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
  "$timestamp" "$epoch" "$available_pct" "$available_gib" "$compressed_gib" \
  "${swap_mib:-}" "${disk_free_gib:-}" "$load_values" "$process_metrics" >> "$data_file"
