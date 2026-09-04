#!/usr/bin/python3
"""Synthetic stdio server; never reads authentication, accounts or local sessions."""
import json
import os
import sys
import time

mode = os.path.basename(sys.argv[0])
for line in sys.stdin:
    request = json.loads(line)
    request_id = request.get("id")
    method = request.get("method")
    if mode == "eof":
        break
    if mode == "timeout":
        time.sleep(20)
        continue
    if request_id is None:
        continue
    if method == "account/rateLimits/read":
        if mode == "legacy" and "params" in request:
            print(json.dumps({"id": request_id, "error": {"code": -32600}}), flush=True)
            continue
        result = {
            "ordinaryUsageAllowed": True,
            "rateLimits": {"secondary": {"usedPercent": 40, "windowDurationMins": 10080, "resetsAt": int(time.time()) + 86400}},
        }
    elif method == "account/read":
        result = {"account": {"planType": "pro"}}
    elif method == "account/usage/read":
        result = {"dailyUsageBuckets": [], "summary": {}}
    elif method == "thread/list":
        result = {"data": []}
    else:
        result = {}
    print(json.dumps({"id": request_id, "result": result}), flush=True)
