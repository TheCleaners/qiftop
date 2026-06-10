# RPM %postun scriptlet for qiftop-agent.
# RPM semantics: $1 == 0 on final removal, $1 >= 1 on upgrade (the new
# version's files are already in place, so leave the service running).

if [ "$1" = "0" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl stop qiftop-agent.service >/dev/null 2>&1 || true
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi
fi

exit 0
