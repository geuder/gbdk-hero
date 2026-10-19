"""Build the phone-playable browser prototype: fills the tile data into template.html.

    python3 build.py        writes stealth.html next to this script
"""
import json, os
here = os.path.dirname(os.path.abspath(__file__))
d = json.load(open(os.path.join(here, 'webdata.json')))
anim = d['anim'] if isinstance(d['anim'], str) else json.dumps(d['anim'])
page = open(os.path.join(here, 'template.html')).read() \
    .replace('__SPR__', d['spr']).replace('__BG__', d['bg']).replace('__ANIM__', anim)
HEAD = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"></head><body style="margin:0">')
open(os.path.join(here, 'stealth.html'), 'w').write(HEAD + page + '</body></html>')
