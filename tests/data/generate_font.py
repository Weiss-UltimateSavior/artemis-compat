"""Generate an original rectangular test glyph; contains no third-party font."""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
fb=FontBuilder(1000,isTTF=True)
fb.setupGlyphOrder(['.notdef','A','space'])
fb.setupCharacterMap({65:'A',32:'space'})
glyphs={}
for name in ['.notdef','A','space']:
    pen=TTGlyphPen(None)
    if name!='space':
        pen.moveTo((0,0));pen.lineTo((600,0));pen.lineTo((600,700));pen.lineTo((0,700));pen.closePath()
    glyphs[name]=pen.glyph()
fb.setupGlyf(glyphs)
fb.setupHorizontalMetrics({'.notdef':(700,0),'A':(700,0),'space':(300,0)})
fb.setupHorizontalHeader(ascent=800,descent=-200)
fb.setupNameTable({'familyName':'Artemis Regression','styleName':'Regular','uniqueFontIdentifier':'ArtemisRegression','fullName':'Artemis Regression','psName':'ArtemisRegression'})
fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
fb.setupPost();fb.setupMaxp()
fb.save(Path(__file__).with_name('rectangle.ttf'))
