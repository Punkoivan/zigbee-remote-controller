import os, sys, json, asyncio, websockets
import ssl; SSL = ssl.create_default_context(); SSL.check_hostname = False; SSL.verify_mode = ssl.CERT_NONE
URL = os.environ["HA_URL"].replace("https", "wss", 1) + "/api/websocket"

async def connect():
    ws = await websockets.connect(URL, max_size=None, ssl=SSL if URL.startswith("wss") else None)
    await ws.recv()
    await ws.send(json.dumps({"type": "auth", "access_token": os.environ["HA_TOKEN"]}))
    await ws.recv()
    return ws

async def call(ws, i, msg):
    msg["id"] = i
    await ws.send(json.dumps(msg))
    while True:
        r = json.loads(await ws.recv())
        if r.get("id") == i:
            return r

async def device():
    ws = await connect()
    devs = (await call(ws, 1, {"type": "config/device_registry/list"}))["result"]
    d = [x for x in devs if x.get("manufacturer") == "punka"]
    for x in d:
        print(json.dumps({k: x.get(k) for k in ("id", "name", "manufacturer", "model", "sw_version", "identifiers")}, ensure_ascii=False))
    if d:
        ents = (await call(ws, 2, {"type": "config/entity_registry/list"}))["result"]
        st = {s["entity_id"]: s for s in (await call(ws, 3, {"type": "get_states"}))["result"]}
        for e in ents:
            if e.get("device_id") == d[0]["id"]:
                s = st.get(e["entity_id"], {})
                print(e["entity_id"], s.get("state"), s.get("attributes", {}).get("unit_of_measurement", ""))
    await ws.close()

async def events(seconds):
    ws = await connect()
    await call(ws, 1, {"type": "subscribe_events", "event_type": "zha_event"})
    try:
        async with asyncio.timeout(seconds):
            while True:
                print(json.dumps(json.loads(await ws.recv())["event"]["data"], ensure_ascii=False), flush=True)
    except TimeoutError:
        pass

if sys.argv[1] == "device":
    asyncio.run(device())
else:
    asyncio.run(events(int(sys.argv[2])))
