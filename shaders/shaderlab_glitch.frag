#version 300 es
// Shader Lab generated typography shader. Edit freely.
// @param glow float 0.0 8.0 2.0
// @param distortion float 0.0 1.0 0.12
// @param scanlines float 0.0 1.0 0.35
uniform vec2 uResolution;
uniform float uTime;
uniform float uDeltaTime;
uniform float uProgress;
uniform float uBPM;
uniform float uBeat;
uniform float uBar;
uniform float uBeatPhase;
uniform float uAudioLevel;
uniform float uBass;
uniform float uMid;
uniform float uTreble;
uniform float uKick;
uniform float uSnare;
uniform sampler2D uText;
uniform vec2 uMouse;
uniform vec4 uColor;
uniform vec4 uColor2;
uniform float uIntensity;
uniform float uSpeed;
uniform float uScale;
uniform float uGlow;
uniform float uDistortion;
uniform float uScanlines;
uniform vec2 uTextPos;
uniform float uTextScale;
uniform float uTextRotation;
uniform float uTextOpacity;
in vec2 vUV;
out vec4 fragColor;

float hash12(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise2(vec2 p) { vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f); float a=hash12(i), b=hash12(i+vec2(1,0)), c=hash12(i+vec2(0,1)), d=hash12(i+vec2(1,1)); return mix(mix(a,b,f.x),mix(c,d,f.x),f.y); }
vec3 hsv(float h, float sat, float val) { vec3 k=vec3(1.0,2.0/3.0,1.0/3.0); vec3 p=abs(fract(vec3(h)+k)*6.0-3.0); return val*mix(k.xxx,clamp(p-1.0,0.0,1.0),sat); }
float glyph(vec2 p, vec2 origin, vec2 size, int code) { vec2 q=(p-origin)/size; if(any(lessThan(q,vec2(0))) || any(greaterThan(q,vec2(1)))) return 0.0; vec2 cell=vec2(float(code % 16),float(code / 16)); q.y=1.0-q.y; return texture(uText,(cell+q)/vec2(16.0,8.0)).a; }
float textMask(vec2 uv) {
  vec2 p=uv; float c=cos(-uTextRotation), d=sin(-uTextRotation); p=(mat2(c,-d,d,c)*(p-vec2(0.5))/max(0.001,uTextScale))+vec2(0.5)-uTextPos; float m=0.0;
  m=max(m,glyph(p,vec2(0.123148,0.5),vec2(0.0685185,0.0685185),78));
  m=max(m,glyph(p,vec2(0.191667,0.5),vec2(0.0685185,0.0685185),85));
  m=max(m,glyph(p,vec2(0.260185,0.5),vec2(0.0685185,0.0685185),76));
  m=max(m,glyph(p,vec2(0.328704,0.5),vec2(0.0685185,0.0685185),76));
  m=max(m,glyph(p,vec2(0.397222,0.5),vec2(0.0685185,0.0685185),32));
  m=max(m,glyph(p,vec2(0.465741,0.5),vec2(0.0685185,0.0685185),83));
  m=max(m,glyph(p,vec2(0.534259,0.5),vec2(0.0685185,0.0685185),69));
  m=max(m,glyph(p,vec2(0.602778,0.5),vec2(0.0685185,0.0685185),67));
  m=max(m,glyph(p,vec2(0.671296,0.5),vec2(0.0685185,0.0685185),84));
  m=max(m,glyph(p,vec2(0.739815,0.5),vec2(0.0685185,0.0685185),79));
  m=max(m,glyph(p,vec2(0.808333,0.5),vec2(0.0685185,0.0685185),82));
  m=max(m,glyph(p,vec2(-0.0138889,0.421204),vec2(0.0685185,0.0685185),68));
  m=max(m,glyph(p,vec2(0.0546296,0.421204),vec2(0.0685185,0.0685185),73));
  m=max(m,glyph(p,vec2(0.123148,0.421204),vec2(0.0685185,0.0685185),71));
  m=max(m,glyph(p,vec2(0.191667,0.421204),vec2(0.0685185,0.0685185),73));
  m=max(m,glyph(p,vec2(0.260185,0.421204),vec2(0.0685185,0.0685185),84));
  m=max(m,glyph(p,vec2(0.328704,0.421204),vec2(0.0685185,0.0685185),65));
  m=max(m,glyph(p,vec2(0.397222,0.421204),vec2(0.0685185,0.0685185),76));
  m=max(m,glyph(p,vec2(0.465741,0.421204),vec2(0.0685185,0.0685185),32));
  m=max(m,glyph(p,vec2(0.534259,0.421204),vec2(0.0685185,0.0685185),72));
  m=max(m,glyph(p,vec2(0.602778,0.421204),vec2(0.0685185,0.0685185),79));
  m=max(m,glyph(p,vec2(0.671296,0.421204),vec2(0.0685185,0.0685185),82));
  m=max(m,glyph(p,vec2(0.739815,0.421204),vec2(0.0685185,0.0685185),73));
  m=max(m,glyph(p,vec2(0.808333,0.421204),vec2(0.0685185,0.0685185),90));
  m=max(m,glyph(p,vec2(0.876852,0.421204),vec2(0.0685185,0.0685185),79));
  m=max(m,glyph(p,vec2(0.94537,0.421204),vec2(0.0685185,0.0685185),78));
  return m*uTextOpacity; }

void main() {
  vec2 uv=vUV;
  float scroll=0.0;
  vec2 q=uv+vec2(scroll,0.0);
  float mask=textMask(q);
  float e=0.0;
  e=max(e,textMask(q+vec2(0.002,0))); e=max(e,textMask(q-vec2(0.002,0)));
  e=max(e,textMask(q+vec2(0,0.003))); e=max(e,textMask(q-vec2(0,0.003)));
  float edge=max(0.0,e-mask);
  float bass=uBass, kick=uKick, energy=uAudioLevel;
  vec3 bg=vec3(0.004,0.008,0.018)+0.018*hsv(0.60+uBar*0.025,0.75,1.0);
  bg += 0.035*vec3(noise2(q*18.0+uTime*uSpeed*0.15));
  vec3 col=uColor.rgb;
  float tear=(hash12(vec2(floor(q.y*18.0),floor(uTime*12.0)))-0.5)*uDistortion; mask=max(mask,textMask(q+vec2(tear,0))); col=vec3(mask,mask*0.35,mask*1.3)+vec3(kick,0.0,kick);
  col += uColor2.rgb*edge*uGlow*1.8;
  col *= 0.85+0.18*energy+0.22*kick;
  float lines=0.92+0.08*sin(uv.y*uResolution.y*0.55); col*=mix(1.0,lines,uScanlines);
  col += vec3(0.02,0.04,0.08)*smoothstep(0.75,0.1,length(uv-0.5));
  fragColor=vec4(bg+col*max(mask,edge*0.25),1.0);
}
