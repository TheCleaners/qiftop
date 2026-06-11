# fish completion for qiftop

complete -c qiftop -s h -l help -d 'Display help'
complete -c qiftop -l help-all -d 'Display help including Qt-specific options'
complete -c qiftop -s v -l version -d 'Display version'
complete -c qiftop -l verbose -d 'Print diagnostic information to stderr'
complete -c qiftop -l no-agent -d 'Skip the system-bus agent and use the in-process backend'
complete -c qiftop -s i -l interface -x -d 'Restrict Connections view to an interface'
complete -c qiftop -l tray -d 'Start with only the system tray icon visible'
