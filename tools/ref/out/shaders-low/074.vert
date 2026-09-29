#define SHADER_TYPE RawShaderMaterial
#define SHADER_NAME OutputShader
#define SRGB_TRANSFER 
#define ACES_FILMIC_TONE_MAPPING 

		precision highp float;

		uniform mat4 modelViewMatrix;
		uniform mat4 projectionMatrix;

		attribute vec3 position;
		attribute vec2 uv;

		varying vec2 vUv;

		void main() {

			vUv = uv;
			gl_Position = projectionMatrix * modelViewMatrix * vec4( position, 1.0 );

		}