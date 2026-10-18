import json, os
body = open('/tmp/pr_sdl31_body.md').read()
payload = {'title':'GB-031 increment: reproducible SDL2 static-musl build profile (tools/sdl)','head':'ticket/31-sdl-profile','base':'main','body':body}
open('/tmp/pr_sdl31_payload.json','w').write(json.dumps(payload))
print('ok', os.path.getsize('/tmp/pr_sdl31_payload.json'))
