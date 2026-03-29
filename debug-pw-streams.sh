#!/bin/bash
# Run this while a Telegram/Teams call is active to see what PipeWire streams exist
echo "=== All Audio Streams ==="
pw-dump | python3 -c "
import sys, json
nodes = json.load(sys.stdin)
for n in nodes:
    if n.get('type') != 'PipeWire:Interface:Node':
        continue
    props = n.get('info', {}).get('props', {})
    mclass = props.get('media.class', '')
    if 'Stream' in mclass or 'Audio' in mclass:
        print(f'  id={n[\"id\"]}')
        print(f'    class  = {mclass}')
        print(f'    app    = {props.get(\"application.name\", \"\")}')
        print(f'    node   = {props.get(\"node.name\", \"\")}')
        print(f'    role   = {props.get(\"media.role\", \"\")}')
        print(f'    binary = {props.get(\"application.process.binary\", \"\")}')
        print(f'    icon   = {props.get(\"application.icon-name\", \"\")}')
        print()
"
