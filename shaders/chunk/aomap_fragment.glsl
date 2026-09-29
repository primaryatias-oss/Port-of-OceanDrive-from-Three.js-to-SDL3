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