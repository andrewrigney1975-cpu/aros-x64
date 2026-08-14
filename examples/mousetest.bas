COLOR 16777215
LOCATE 2, 2
PRINT "MOUSE TEST - move the mouse and click buttons. Press Escape to quit."
WHILE 1
  IF KEYHIT THEN
    LET k = GETKEY
    IF k = 136 THEN END
  ENDIF

  LOCATE 4, 2
  PRINT "X: ", MOUSEX, "        "
  LOCATE 5, 2
  PRINT "Y: ", MOUSEY, "        "
  LOCATE 6, 2
  PRINT "BTN: ", MOUSEBTN, "        "
  LOCATE 7, 2
  PRINT "IRQ COUNT:  ", MIRQCNT, "        "
  LOCATE 8, 2
  PRINT "PKT COUNT:  ", MPKTCNT, "        "
  LOCATE 9, 2
  PRINT "SYNC DROPS: ", MDROPCNT, "        "
  WAIT 5
WEND
