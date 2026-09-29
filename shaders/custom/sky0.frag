
      varying vec3 vDir;
      ${SKY_FULL_GLSL}
      void main() {
        gl_FragColor = vec4(odSky(normalize(vDir), 0.0), 1.0);
      }