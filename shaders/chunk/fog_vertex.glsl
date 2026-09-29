
#ifdef USE_FOG
  vFogOffset = (vec4(mvPosition.xyz, 0.0) * viewMatrix).xyz;
#endif