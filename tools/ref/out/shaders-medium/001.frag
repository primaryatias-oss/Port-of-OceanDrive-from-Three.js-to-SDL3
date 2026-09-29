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
#define SHADER_TYPE MeshBasicMaterial
#define SHADER_NAME PMREM.Background
#define FLIP_SIDED
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

uniform vec3 diffuse;
uniform float opacity;
#ifndef FLAT_SHADED
	varying vec3 vNormal;
#endif
#define PI 3.141592653589793
#define PI2 6.283185307179586
#define PI_HALF 1.5707963267948966
#define RECIPROCAL_PI 0.3183098861837907
#define RECIPROCAL_PI2 0.15915494309189535
#define EPSILON 1e-6
#ifndef saturate
#define saturate( a ) clamp( a, 0.0, 1.0 )
#endif
#define whiteComplement( a ) ( 1.0 - saturate( a ) )
float pow2( const in float x ) { return x*x; }
vec3 pow2( const in vec3 x ) { return x*x; }
float pow3( const in float x ) { return x*x*x; }
float pow4( const in float x ) { float x2 = x*x; return x2*x2; }
float max3( const in vec3 v ) { return max( max( v.x, v.y ), v.z ); }
float average( const in vec3 v ) { return dot( v, vec3( 0.3333333 ) ); }
highp float rand( const in vec2 uv ) {
	const highp float a = 12.9898, b = 78.233, c = 43758.5453;
	highp float dt = dot( uv.xy, vec2( a,b ) ), sn = mod( dt, PI );
	return fract( sin( sn ) * c );
}
#ifdef HIGH_PRECISION
	float precisionSafeLength( vec3 v ) { return length( v ); }
#else
	float precisionSafeLength( vec3 v ) {
		float maxComponent = max3( abs( v ) );
		return length( v / maxComponent ) * maxComponent;
	}
#endif
struct IncidentLight {
	vec3 color;
	vec3 direction;
	bool visible;
};
struct ReflectedLight {
	vec3 directDiffuse;
	vec3 directSpecular;
	vec3 indirectDiffuse;
	vec3 indirectSpecular;
};
#ifdef USE_ALPHAHASH
	varying vec3 vPosition;
#endif
vec3 transformDirection( in vec3 dir, in mat4 matrix ) {
	return normalize( ( matrix * vec4( dir, 0.0 ) ).xyz );
}
#define inverseTransformDirection transformDirectionByInverseViewMatrix
vec3 transformNormalByInverseViewMatrix( in vec3 normal, in mat4 viewMatrix ) {
	return normalize( ( vec4( normal, 0.0 ) * viewMatrix ).xyz );
}
vec3 transformDirectionByInverseViewMatrix( in vec3 dir, in mat4 viewMatrix ) {
	return normalize( ( vec4( dir, 0.0 ) * viewMatrix ).xyz );
}
bool isPerspectiveMatrix( mat4 m ) {
	return m[ 2 ][ 3 ] == - 1.0;
}
vec2 equirectUv( in vec3 dir ) {
	float u = atan( dir.z, dir.x ) * RECIPROCAL_PI2 + 0.5;
	float v = asin( clamp( dir.y, - 1.0, 1.0 ) ) * RECIPROCAL_PI + 0.5;
	return vec2( u, v );
}
vec3 BRDF_Lambert( const in vec3 diffuseColor ) {
	return RECIPROCAL_PI * diffuseColor;
}
vec3 F_Schlick( const in vec3 f0, const in float f90, const in float dotVH ) {
	float fresnel = exp2( ( - 5.55473 * dotVH - 6.98316 ) * dotVH );
	return f0 * ( 1.0 - fresnel ) + ( f90 * fresnel );
}
float F_Schlick( const in float f0, const in float f90, const in float dotVH ) {
	float fresnel = exp2( ( - 5.55473 * dotVH - 6.98316 ) * dotVH );
	return f0 * ( 1.0 - fresnel ) + ( f90 * fresnel );
} // validated
#ifdef DITHERING
	vec3 dithering( vec3 color ) {
		float grid_position = rand( gl_FragCoord.xy );
		vec3 dither_shift_RGB = vec3( 0.25 / 255.0, -0.25 / 255.0, 0.25 / 255.0 );
		dither_shift_RGB = mix( 2.0 * dither_shift_RGB, -2.0 * dither_shift_RGB, grid_position );
		return color + dither_shift_RGB;
	}
#endif
#if defined( USE_COLOR ) || defined( USE_COLOR_ALPHA )
	varying vec4 vColor;
#endif
#if defined( USE_UV ) || defined( USE_ANISOTROPY )
	varying vec2 vUv;
#endif
#ifdef USE_MAP
	varying vec2 vMapUv;
#endif
#ifdef USE_ALPHAMAP
	varying vec2 vAlphaMapUv;
#endif
#ifdef USE_LIGHTMAP
	varying vec2 vLightMapUv;
#endif
#ifdef USE_AOMAP
	varying vec2 vAoMapUv;
#endif
#ifdef USE_BUMPMAP
	varying vec2 vBumpMapUv;
#endif
#ifdef USE_NORMALMAP
	varying vec2 vNormalMapUv;
#endif
#ifdef USE_EMISSIVEMAP
	varying vec2 vEmissiveMapUv;
#endif
#ifdef USE_METALNESSMAP
	varying vec2 vMetalnessMapUv;
#endif
#ifdef USE_ROUGHNESSMAP
	varying vec2 vRoughnessMapUv;
#endif
#ifdef USE_ANISOTROPYMAP
	varying vec2 vAnisotropyMapUv;
#endif
#ifdef USE_CLEARCOATMAP
	varying vec2 vClearcoatMapUv;
#endif
#ifdef USE_CLEARCOAT_NORMALMAP
	varying vec2 vClearcoatNormalMapUv;
#endif
#ifdef USE_CLEARCOAT_ROUGHNESSMAP
	varying vec2 vClearcoatRoughnessMapUv;
#endif
#ifdef USE_IRIDESCENCEMAP
	varying vec2 vIridescenceMapUv;
#endif
#ifdef USE_IRIDESCENCE_THICKNESSMAP
	varying vec2 vIridescenceThicknessMapUv;
#endif
#ifdef USE_SHEEN_COLORMAP
	varying vec2 vSheenColorMapUv;
#endif
#ifdef USE_SHEEN_ROUGHNESSMAP
	varying vec2 vSheenRoughnessMapUv;
#endif
#ifdef USE_SPECULARMAP
	varying vec2 vSpecularMapUv;
#endif
#ifdef USE_SPECULAR_COLORMAP
	varying vec2 vSpecularColorMapUv;
#endif
#ifdef USE_SPECULAR_INTENSITYMAP
	varying vec2 vSpecularIntensityMapUv;
#endif
#ifdef USE_TRANSMISSIONMAP
	uniform mat3 transmissionMapTransform;
	varying vec2 vTransmissionMapUv;
#endif
#ifdef USE_THICKNESSMAP
	uniform mat3 thicknessMapTransform;
	varying vec2 vThicknessMapUv;
#endif
#ifdef USE_MAP
	uniform sampler2D map;
#endif
#ifdef USE_ALPHAMAP
	uniform sampler2D alphaMap;
#endif
#ifdef USE_ALPHATEST
	uniform float alphaTest;
#endif
#ifdef USE_ALPHAHASH
	const float ALPHA_HASH_SCALE = 0.05;
	float hash2D( vec2 value ) {
		return fract( 1.0e4 * sin( 17.0 * value.x + 0.1 * value.y ) * ( 0.1 + abs( sin( 13.0 * value.y + value.x ) ) ) );
	}
	float hash3D( vec3 value ) {
		return hash2D( vec2( hash2D( value.xy ), value.z ) );
	}
	float getAlphaHashThreshold( vec3 position ) {
		float maxDeriv = max(
			length( dFdx( position.xyz ) ),
			length( dFdy( position.xyz ) )
		);
		float pixScale = 1.0 / ( ALPHA_HASH_SCALE * maxDeriv );
		vec2 pixScales = vec2(
			exp2( floor( log2( pixScale ) ) ),
			exp2( ceil( log2( pixScale ) ) )
		);
		vec2 alpha = vec2(
			hash3D( floor( pixScales.x * position.xyz ) ),
			hash3D( floor( pixScales.y * position.xyz ) )
		);
		float lerpFactor = fract( log2( pixScale ) );
		float x = ( 1.0 - lerpFactor ) * alpha.x + lerpFactor * alpha.y;
		float a = min( lerpFactor, 1.0 - lerpFactor );
		vec3 cases = vec3(
			x * x / ( 2.0 * a * ( 1.0 - a ) ),
			( x - 0.5 * a ) / ( 1.0 - a ),
			1.0 - ( ( 1.0 - x ) * ( 1.0 - x ) / ( 2.0 * a * ( 1.0 - a ) ) )
		);
		float threshold = ( x < ( 1.0 - a ) )
			? ( ( x < a ) ? cases.x : cases.y )
			: cases.z;
		return clamp( threshold , 1.0e-6, 1.0 );
	}
#endif
#ifdef USE_AOMAP
	uniform sampler2D aoMap;
	uniform float aoMapIntensity;
#endif
#ifdef USE_LIGHTMAP
	uniform sampler2D lightMap;
	uniform float lightMapIntensity;
#endif
#ifdef USE_ENVMAP
	uniform float envMapIntensity;
	uniform mat3 envMapRotation;
	#ifdef ENVMAP_TYPE_CUBE
		uniform samplerCube envMap;
	#else
		uniform sampler2D envMap;
	#endif
#endif
#ifdef USE_ENVMAP
	uniform float reflectivity;
	#if defined( USE_BUMPMAP ) || defined( USE_NORMALMAP ) || defined( PHONG ) || defined( LAMBERT )
		#define ENV_WORLDPOS
	#endif
	#ifdef ENV_WORLDPOS
		varying vec3 vWorldPosition;
		uniform float refractionRatio;
	#else
		varying vec3 vReflect;
	#endif
#endif

#ifdef USE_FOG
  uniform vec3 fogColor;
  varying vec3 vFogOffset;
  uniform float fogDensity;
  
const vec3 OD_SUN = vec3(0.97747, 0.12187, 0.17235);
const vec3 OD_SUNCOL = vec3(1.0000, 0.6000, 0.4000);
const float OD_SUN_I = 5.100;

float odSunSide(vec3 d) {
  vec2 hd = d.xz / max(length(d.xz), 1e-4);
  vec2 hs = normalize(OD_SUN.xz);
  return clamp(dot(hd, hs) * 0.5 + 0.5, 0.0, 1.0); // 1 toward the sun, 0 opposite
}

// Sun glow: Henyey-Greenstein forward-scattering lobe (g = 0.86) for the broad warm
// hotspot, plus the photographed sun: a blown white-yellow core ~4-5 deg across (lens
// flare of an over-exposed sun) fading through cream into gold; the true disc sits
// hidden inside the clipped core.
vec3 odSunGlow(float mu, float lobe, float core) {
  const float g = 0.86;
  float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * mu, 1.5) * 0.0796;
  float th = sqrt(max(2.0 * (1.0 - mu), 0.0));          // angle from the sun (rad)
  vec3 c = vec3(1.0, 0.50, 0.14) * 0.26 * hg * lobe;
  float disc = 1.0 - smoothstep(0.0095, 0.0125, th);
  c += core * (vec3(1.0, 0.80, 0.46) * 5.5 * exp(-pow(th / 0.028, 1.5))    // clipped core -> cream
             + vec3(1.0, 0.58, 0.20) * 1.2 * exp(-th / 0.07)                // cream -> gold
             + vec3(12.0, 9.0, 5.0) * disc);
  return c;
}

// The glow of a sun on the horizon is flattened: a warm band hugging the horizon,
// much wider than tall (low-level haze layering and refraction).
vec3 odSunBand(vec3 d) {
  float dA = acos(clamp(dot(normalize(d.xz + 1e-5), normalize(OD_SUN.xz)), -1.0, 1.0));
  float dE = max(d.y, 0.0) - OD_SUN.y;
  return vec3(1.0, 0.40, 0.09) * 0.5 * exp(-pow(dA / 0.32, 2.0) - pow(dE / (dE < 0.0 ? 0.06 : 0.045), 2.0));
}

vec3 odSkyBase(vec3 d, float glowScale) {
  float e = max(d.y, 0.0);
  float mu = dot(d, OD_SUN);
  float az = odSunSide(d);

  // Anti-solar side: dusty blue-grey dome, pink "belt" above a blue-grey earth-shadow band.
  // clear pale blue dome, a soft pink band just above the horizon
  vec3 away = mix(vec3(0.400, 0.540, 0.780), vec3(0.250, 0.380, 0.650), smoothstep(0.35, 0.95, e));
  away = mix(vec3(0.600, 0.620, 0.760), away, smoothstep(0.1, 0.32, e));
  away = mix(vec3(0.860, 0.640, 0.660), away, smoothstep(0.02, 0.14, e));
  away = mix(vec3(0.720, 0.640, 0.700), away, smoothstep(0.0, 0.03, e));

  // Solar side, a broad graded band: red-orange at the horizon -> deep orange (~6 deg)
  // -> orange-gold (~15 deg) -> pale yellow -> clean blue-grey.
  vec3 sun = mix(vec3(0.270, 0.420, 0.690), vec3(0.200, 0.300, 0.520), smoothstep(0.35, 0.95, e));
  sun = mix(vec3(0.780, 0.640, 0.420), sun, smoothstep(0.14, 0.50, e));
  sun = mix(vec3(1.050, 0.540, 0.160), sun, smoothstep(0.05, 0.27, e));
  sun = mix(vec3(0.950, 0.300, 0.070), sun, smoothstep(0.0, 0.11, e));

  float sw = pow(az, 1.5 + 4.0 * e);
  vec3 col = mix(away, sun, sw);
  return col + odSunGlow(mu, 0.8, glowScale) + odSunBand(d) * glowScale;
}

// Aerial-perspective colour: the sky just above the horizon in that direction.
vec3 odHaze(vec3 d) {
  vec3 hd = normalize(vec3(d.x, max(d.y, 0.0) * 0.5 + 0.004, d.z));
  // only part of the forward-scattering lobe: a 40 m slab of air toward the sun is not the whole sky
  return odSkyBase(hd, 0.0) - odSunGlow(dot(hd, OD_SUN), 0.7, 0.0);
}

  
vec3 odApplyFog(vec3 col, vec3 offs, float density) {
  float fDist = length(offs);
  vec3 fDir = offs / max(fDist, 1e-4);
  const float fH = 120.0;
  float fDy = offs.y / fH;
  float fK = abs(fDy) > 1e-3 ? (1.0 - exp(-fDy)) / fDy : 1.0;
  // the first ~50 m stay crisp; humid haze builds beyond that
  float fOd = density * exp(-max(cameraPosition.y, 0.0) / fH) * max(fDist - 90.0, 0.0) * mix(0.18, 1.0, smoothstep(250.0, 500.0, fDist)) * fK;
  // The haze colour is the horizon sky, i.e. km of air. Toward the sun that is the
  // blazing glow, so a short slab of it would light up backlit sand as bright as the
  // sky; there the haze is thinned to keep near backlit ground dark as in photos.
  fOd *= mix(1.0, 0.3, smoothstep(0.3, 0.95, dot(fDir, OD_SUN)));
  // down-sun the near air is barely visible: facades 50-150 m away stay crisp and warm
  // (and the sunlit towers behind them, 150-400 m, stay warm rather than lilac boxes)
  fOd *= mix(1.0, 0.45, smoothstep(0.2, 0.8, -dot(normalize(fDir.xz + 1e-5), normalize(OD_SUN.xz))) * (1.0 - smoothstep(100.0, 180.0, fDist)));
  fOd *= mix(1.0, 1.0, smoothstep(0.2, 0.8, -dot(normalize(fDir.xz + 1e-5), normalize(OD_SUN.xz))) * smoothstep(120.0, 220.0, fDist) * (1.0 - smoothstep(350.0, 700.0, fDist)));
  return mix(col, odHaze(fDir), 1.0 - exp(-fOd));
}

#endif
#ifdef USE_SPECULARMAP
	uniform sampler2D specularMap;
#endif
#if defined( USE_LOGARITHMIC_DEPTH_BUFFER )
	uniform float logDepthBufFC;
	varying float vFragDepth;
	varying float vIsPerspective;
#endif
#if 0 > 0
	varying vec3 vClipPosition;
	uniform vec4 clippingPlanes[ 0 ];
#endif
void main() {
	vec4 diffuseColor = vec4( diffuse, opacity );
#if 0 > 0
	vec4 plane;
	#ifdef ALPHA_TO_COVERAGE
		float distanceToPlane, distanceGradient;
		float clipOpacity = 1.0;
		
		#if 0 < 0
			float unionClipOpacity = 1.0;
			
			clipOpacity *= 1.0 - unionClipOpacity;
		#endif
		diffuseColor.a *= clipOpacity;
		if ( diffuseColor.a == 0.0 ) discard;
	#else
		
		#if 0 < 0
			bool clipped = true;
			
			if ( clipped ) discard;
		#endif
	#endif
#endif
#if defined( USE_LOGARITHMIC_DEPTH_BUFFER )
	gl_FragDepth = vIsPerspective == 0.0 ? gl_FragCoord.z : log2( vFragDepth ) * logDepthBufFC * 0.5;
#endif
#ifdef USE_MAP
	vec4 sampledDiffuseColor = texture2D( map, vMapUv );
	#ifdef DECODE_VIDEO_TEXTURE
		sampledDiffuseColor = sRGBTransferEOTF( sampledDiffuseColor );
	#endif
	diffuseColor *= sampledDiffuseColor;
#endif
#if defined( USE_COLOR ) || defined( USE_COLOR_ALPHA )
	diffuseColor *= vColor;
#endif
#ifdef USE_ALPHAMAP
	diffuseColor.a *= texture2D( alphaMap, vAlphaMapUv ).g;
#endif
#ifdef USE_ALPHATEST
	#ifdef ALPHA_TO_COVERAGE
	diffuseColor.a = smoothstep( alphaTest, alphaTest + fwidth( diffuseColor.a ), diffuseColor.a );
	if ( diffuseColor.a == 0.0 ) discard;
	#else
	if ( diffuseColor.a < alphaTest ) discard;
	#endif
#endif
#ifdef USE_ALPHAHASH
	if ( diffuseColor.a < getAlphaHashThreshold( vPosition ) ) discard;
#endif
float specularStrength;
#ifdef USE_SPECULARMAP
	vec4 texelSpecular = texture2D( specularMap, vSpecularMapUv );
	specularStrength = texelSpecular.r;
#else
	specularStrength = 1.0;
#endif
	ReflectedLight reflectedLight = ReflectedLight( vec3( 0.0 ), vec3( 0.0 ), vec3( 0.0 ), vec3( 0.0 ) );
	#ifdef USE_LIGHTMAP
		vec4 lightMapTexel = texture2D( lightMap, vLightMapUv );
		reflectedLight.indirectDiffuse += lightMapTexel.rgb * lightMapIntensity * RECIPROCAL_PI;
	#else
		reflectedLight.indirectDiffuse += vec3( 1.0 );
	#endif
#ifdef USE_AOMAP
	float ambientOcclusion = ( texture2D( aoMap, vAoMapUv ).r - 1.0 ) * aoMapIntensity + 1.0;
	reflectedLight.indirectDiffuse *= ambientOcclusion;
	#if defined( USE_CLEARCOAT ) 
		clearcoatSpecularIndirect *= ambientOcclusion;
	#endif
	#if defined( USE_SHEEN ) 
		sheenSpecularIndirect *= ambientOcclusion;
	#endif
	#if defined( USE_ENVMAP ) && defined( STANDARD )
		float dotNV = saturate( dot( geometryNormal, geometryViewDir ) );
		reflectedLight.indirectSpecular *= computeSpecularOcclusion( dotNV, ambientOcclusion, material.roughness );
	#endif
#endif
#ifdef STANDARD
  {
    vec3 odBn = inverseTransformDirection( normal, viewMatrix );
    reflectedLight.indirectDiffuse += diffuseColor.rgb * vec3( 0.2, 0.14, 0.09 ) * max( -odBn.y, 0.0 );
    // Looking toward the low sun, the faces you see are turned away from it: they get only
    // the dim anti-solar sky, and rough ground seen against the light forward-scatters little
    // (sand, grass and leaves are back-scatterers). Photos read them near-silhouette.
    const vec3 odBs = vec3(0.9775, 0.1219, 0.1724);
    vec3 odVw = inverseTransformDirection( -vViewPosition, viewMatrix );
    float odTow = smoothstep( 0.3, 0.9, dot( normalize( odVw.xz + 1e-5 ), normalize( odBs.xz ) ) );
    float odAwayN = smoothstep( 0.15, -0.45, dot( odBn, odBs ) );
    float odUp = smoothstep( 0.5, 0.9, odBn.y );
    float odK = odTow * max( odAwayN, 0.3 * odUp );
    reflectedLight.indirectDiffuse *= 1.0 - 0.6 * odK;
    reflectedLight.indirectSpecular *= 1.0 - 0.35 * odK;
    reflectedLight.directDiffuse *= 1.0 - 0.3 * odTow * odUp;
    // (no grazing forward-specular sheen off rough ground: grains and blades self-shadow it)
    float odRuf = odTow * odUp * smoothstep( 0.55, 0.8, material.roughness );
    reflectedLight.directSpecular *= 1.0 - 0.85 * odRuf;
    reflectedLight.indirectSpecular *= 1.0 - 0.75 * odRuf;
  }
#endif
	reflectedLight.indirectDiffuse *= diffuseColor.rgb;
	vec3 outgoingLight = reflectedLight.indirectDiffuse;
#ifdef USE_ENVMAP
	#ifdef ENV_WORLDPOS
		vec3 cameraToFrag;
		if ( isOrthographic ) {
			cameraToFrag = normalize( vec3( - viewMatrix[ 0 ][ 2 ], - viewMatrix[ 1 ][ 2 ], - viewMatrix[ 2 ][ 2 ] ) );
		} else {
			cameraToFrag = normalize( vWorldPosition - cameraPosition );
		}
		vec3 worldNormal = transformNormalByInverseViewMatrix( normal, viewMatrix );
		#ifdef ENVMAP_MODE_REFLECTION
			vec3 reflectVec = reflect( cameraToFrag, worldNormal );
		#else
			vec3 reflectVec = refract( cameraToFrag, worldNormal, refractionRatio );
		#endif
	#else
		vec3 reflectVec = vReflect;
	#endif
	#ifdef ENVMAP_TYPE_CUBE
		vec4 envColor = textureCube( envMap, envMapRotation * reflectVec );
		#ifdef ENVMAP_BLENDING_MULTIPLY
			outgoingLight = mix( outgoingLight, outgoingLight * envColor.xyz, specularStrength * reflectivity );
		#elif defined( ENVMAP_BLENDING_MIX )
			outgoingLight = mix( outgoingLight, envColor.xyz, specularStrength * reflectivity );
		#elif defined( ENVMAP_BLENDING_ADD )
			outgoingLight += envColor.xyz * specularStrength * reflectivity;
		#endif
	#endif
#endif
#ifdef OPAQUE
diffuseColor.a = 1.0;
#endif
#ifdef USE_TRANSMISSION
diffuseColor.a *= material.transmissionAlpha;
#endif
gl_FragColor = vec4( outgoingLight, diffuseColor.a );
#if defined( TONE_MAPPING )
	gl_FragColor.rgb = toneMapping( gl_FragColor.rgb );
#endif
gl_FragColor = linearToOutputTexel( gl_FragColor );

#ifdef USE_FOG
  gl_FragColor.rgb = odApplyFog(gl_FragColor.rgb, vFogOffset, fogDensity);
#endif
#ifdef PREMULTIPLIED_ALPHA
	gl_FragColor.rgb *= gl_FragColor.a;
#endif
#ifdef DITHERING
	gl_FragColor.rgb = dithering( gl_FragColor.rgb );
#endif
}