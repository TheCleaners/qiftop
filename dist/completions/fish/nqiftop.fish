# fish completion for nqiftop

complete -c nqiftop -s h -l help -d 'Display help'
complete -c nqiftop -l help-all -d 'Display help including Qt-specific options'
complete -c nqiftop -s v -l version -d 'Display version'
complete -c nqiftop -l session -d 'Talk to the agent on the session bus'
complete -c nqiftop -l no-agent -d 'Skip the agent and capture in-process'
complete -c nqiftop -l verbose -d 'Print diagnostics to stderr'
complete -c nqiftop -s i -l interval -x -d 'Poll interval in milliseconds'
complete -c nqiftop -l theme -x -a 'dark light colorblind mono' -d 'Colour theme'
complete -c nqiftop -l view -x -a 'interfaces connections' -d 'Initial tab'
complete -c nqiftop -l group -x -a 'off none flat interface iface if process proc container ctr' -d 'Group connections'
