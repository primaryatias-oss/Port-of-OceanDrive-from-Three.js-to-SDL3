
      varying vec3 vCol; varying float vA;
      void main() {
        vec2 c = gl_PointCoord * 2.0 - 1.0;
        float r2 = dot(c, c);
        if (r2 > 1.0) discard;
        gl_FragColor = vec4(vCol, vA * pow(1.0 - r2, 1.5));
      }