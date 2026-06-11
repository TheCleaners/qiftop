# fish completion for qiftop-agent

complete -c qiftop-agent -s h -l help -d 'Display help'
complete -c qiftop-agent -l help-all -d 'Display help including Qt-specific options'
complete -c qiftop-agent -s v -l version -d 'Display version'
complete -c qiftop-agent -l verbose -d 'Enable verbose tracing on stderr'
complete -c qiftop-agent -l session -d 'Use the session bus instead of the system bus'
complete -c qiftop-agent -s c -l config -r -F -d 'Path to agent.conf'
