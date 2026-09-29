
      attribute vec3 aCol; attribute float aA; attribute float aS;
      uniform float uScale;
      varying vec3 vCol; varying float vA;
      void main() {
        vec4 mv = modelViewMatrix * vec4(position, 1.0);
        gl_Position = projectionMatrix * mv;
        gl_PointSize = aA > 0.001 ? clamp(aS * uScale / max(-mv.z, 0.1), 1.0, 90.0) : 0.0;
        vCol = aCol; vA = aA;
      }