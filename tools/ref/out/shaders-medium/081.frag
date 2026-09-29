#version 300 es
#define varying in
layout(location = 0) out highp vec4 pc_fragColor;
#define gl_FragColor pc_fragColor
#define gl_FragDepthEXT gl_FragDepth
#define texture2D texture
#define textureCube texture
#define texture2DProj textureProj
#define texture2DLodEXT textureLod
#define texture2DProjLodEXT textureProjLod
#define textureCubeLodEXT textureLod
#define texture2DGradEXT textureGrad
#define texture2DProjGradEXT textureProjGrad
#define textureCubeGradEXT textureGrad
precision highp float;
	precision highp int;
	precision highp sampler2D;
	precision highp samplerCube;
	precision highp sampler3D;
	precision highp sampler2DArray;
	precision highp sampler2DShadow;
	precision highp samplerCubeShadow;
	precision highp sampler2DArrayShadow;
	precision highp isampler2D;
	precision highp isampler3D;
	precision highp isamplerCube;
	precision highp isampler2DArray;
	precision highp usampler2D;
	precision highp usampler3D;
	precision highp usamplerCube;
	precision highp usampler2DArray;
	
#define HIGH_PRECISION
#define SHADER_TYPE ShaderMaterial
#define SHADER_NAME CameraGrade
uniform mat4 viewMatrix;
uniform vec3 cameraPosition;
uniform bool isOrthographic;
#define OPAQUE
vec4 LinearTransferOETF( in vec4 value ) {
	return value;
}
vec4 sRGBTransferEOTF( in vec4 value ) {
	return vec4( mix( pow( value.rgb * 0.9478672986 + vec3( 0.0521327014 ), vec3( 2.4 ) ), value.rgb * 0.0773993808, vec3( lessThanEqual( value.rgb, vec3( 0.04045 ) ) ) ), value.a );
}
vec4 sRGBTransferOETF( in vec4 value ) {
	return vec4( mix( pow( value.rgb, vec3( 0.41666 ) ) * 1.055 - vec3( 0.055 ), value.rgb * 12.92, vec3( lessThanEqual( value.rgb, vec3( 0.0031308 ) ) ) ), value.a );
}
vec4 linearToOutputTexel( vec4 value ) {
	return LinearTransferOETF( vec4( value.rgb * mat3( 1.0000,-0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000 ), value.a ) );
}
float luminance( const in vec3 rgb ) {
	const vec3 weights = vec3( 0.2126, 0.7152, 0.0722 );
	return dot( weights, rgb );
}


    uniform sampler2D tDiffuse;
    uniform float uTime, uSaturation;
    uniform vec2 uResolution, uSunUv;
    uniform float uSunVis;
    uniform vec3 uShadowTint, uHighlightTint;
    varying vec2 vUv;

    float hash(vec3 p) {
      p = fract(p * 0.1031);
      p += dot(p, p.zyx + 31.32);
      return fract((p.x + p.y) * p.z);
    }

    void main() {
      vec3 c = texture2D(tDiffuse, vUv).rgb;
      // veiling glare from an in-frame low sun: lifts and flattens everything around it
      vec2 sd = (vUv - uSunUv) * vec2(uResolution.x / uResolution.y, 1.0);
      float dsun = length(sd);
      // flattened: the glare spreads along the horizon more than up and down
      float dsE = length(sd * vec2(0.55, 1.6));
      c += vec3(1.0, 0.46, 0.1) * uSunVis * (0.05 * exp(-dsE * 9.0) + 0.045 * exp(-dsE * 3.5));
      // lens character: faint irregular rays, a soft horizontal streak, two ghosts
      // mirrored through the frame centre, and a low-contrast veil over the frame
      float ang = atan(sd.y, sd.x);
      float rays = 0.55 + 0.45 * sin(ang * 7.0 + 1.3) * sin(ang * 11.0 - 0.4);
      c += vec3(1.0, 0.62, 0.3) * uSunVis * 0.012 * rays * exp(-dsun * 6.0);
      c += vec3(1.0, 0.55, 0.25) * uSunVis * 0.035 * exp(-abs(sd.y) * 70.0) * exp(-abs(sd.x) * 2.5);
      vec2 aspect = vec2(uResolution.x / uResolution.y, 1.0);
      vec2 g1 = 0.5 + (0.5 - uSunUv) * 0.55, g2 = 0.5 + (0.5 - uSunUv) * 1.25;
      c += vec3(0.55, 0.75, 0.45) * uSunVis * 0.018 * smoothstep(0.055, 0.035, length((vUv - g1) * aspect));
      c += vec3(0.9, 0.5, 0.3) * uSunVis * 0.012 * smoothstep(0.11, 0.06, length((vUv - g2) * aspect));
      c += vec3(0.007, 0.005, 0.0035) * uSunVis;
      // split tone: cool shade, warm light (one consistent sunrise balance)
      float l0 = dot(c, vec3(0.2126, 0.7152, 0.0722));
      c *= mix(uShadowTint, uHighlightTint, smoothstep(0.015, 0.15, l0));

      float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
      // sunlit highlights keep their colour (ACES would otherwise bleach them to pastel)
      c = max(mix(vec3(l), c, uSaturation + 0.02 * smoothstep(0.2, 1.2, l)), 0.0);
      // lift the deepest shadows very slightly toward a cool tone (film toe)
      c += vec3(0.004, 0.006, 0.010) * (1.0 - smoothstep(0.0, 0.08, l));

      gl_FragColor = vec4(c, 1.0);
    }