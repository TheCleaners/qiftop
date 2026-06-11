# fish completion for check_qiftop

complete -c check_qiftop -l warning -x -d 'Warning threshold byte rate'
complete -c check_qiftop -l critical -x -d 'Critical threshold byte rate'
complete -c check_qiftop -l metric -x -a 'rate_in rx_rate rate_rx rx in rate_out tx_rate rate_tx tx out rate_total total_rate rate total' -d 'Metric to evaluate'
complete -c check_qiftop -l iface -x -d 'Check one interface by name'
complete -c check_qiftop -l filter -x -d 'Connection filter expression'
complete -c check_qiftop -l session -d 'Use the session bus'
complete -c check_qiftop -l timeout -x -d 'Timeout in seconds'
complete -c check_qiftop -l interval-ms -x -d 'Poll interval in milliseconds'
