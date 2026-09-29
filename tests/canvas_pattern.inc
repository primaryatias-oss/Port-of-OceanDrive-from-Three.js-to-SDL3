// Shared canvas test pattern: included by tests/test_canvas.c (C API) and turned into JS by
// tools/ref/canvas-ref.mjs (each line maps 1:1 onto a Canvas 2D call).
// Format: one call per line, `op arg arg ...`; strings in double quotes.
fillColor "#f3efe4"
fillRect 0 0 512 400
fillColor "#1f6b45"
fillRect 10.5 10.25 120 60
strokeColor "#f2f2ea"
lineWidth 6
strokeRect 18 18 104 44
fillColor "rgba(200,40,40,0.55)"
beginPath
moveTo 150 12
lineTo 250 30
lineTo 210 90
lineTo 160 70
closePath
fill
strokeColor "#222"
lineWidth 8
lineJoin miter
beginPath
moveTo 270 20
lineTo 330 80
lineTo 390 20
stroke
lineJoin round
beginPath
moveTo 400 20
lineTo 440 80
lineTo 480 20
stroke
lineCap round
lineWidth 12
beginPath
moveTo 30 110
lineTo 120 150
stroke
lineCap butt
lineWidth 3
beginPath
arc 190 140 38 0 6.2831853 false
stroke
fillColor "#3d7d4e"
beginPath
ellipse 300 140 60 28 0.4 0 6.2831853 false
fill
strokeColor "rgba(70,60,50,0.8)"
lineWidth 4
beginPath
moveTo 380 110
bezierTo 420 180 460 90 500 160
stroke
linearGradient 10 200 250 200
stop 0 "#ffffff"
stop 0.5 "rgba(255,120,40,0.6)"
stop 1 "#0050a0"
fillGradient
fillRect 10 190 240 60
radialGradient 380 220 5 380 220 70
stop 0 "rgba(255,255,255,0.9)"
stop 1 "rgba(255,255,255,0)"
fillGradient
fillRect 300 180 160 90
radialGradient 120 330 10 150 320 60
stop 0 "#e0a33a"
stop 1 "#2e7fa8"
fillGradient
fillRect 60 270 130 110
save
beginPath
rect 220 280 80 80
clip
fillColor "#b3241e"
beginPath
arc 260 320 55 0 6.2831853 false
fill
restore
save
filterBlur 3
fillColor "#2f2a26"
fillRect 330 290 60 60
restore
save
translate 450 330
scale 0.8 1.3
fillColor "rgba(40,90,160,0.7)"
fillRect -30 -30 60 40
restore
putImageData 20 385 40 10
