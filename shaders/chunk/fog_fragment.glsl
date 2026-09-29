
#ifdef USE_FOG
  gl_FragColor.rgb = odApplyFog(gl_FragColor.rgb, vFogOffset, fogDensity);
#endif