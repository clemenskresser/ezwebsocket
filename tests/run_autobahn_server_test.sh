#!/bin/bash
docker run -it --rm -v "${PWD}/config:/config" -v "${PWD}/reports:/reports" --name fuzzingclient --network=host crossbario/autobahn-testsuite  wstest -m fuzzingclient -s /config/fuzzingclient.json
