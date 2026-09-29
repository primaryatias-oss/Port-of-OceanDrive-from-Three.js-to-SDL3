
      varying vec3 vDir;
      ${SKY_FULL_GLSL}
      void main() {
        gl_FragColor = vec4(odSky(normalize(vDir), 1.0), 1.0);
      }