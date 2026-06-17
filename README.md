python3 ./gerber_tool.py 
Usage: gerber_tool.py --svg  board.GTO [output.svg] [--outline board.GKO]
       gerber_tool.py --gerber board.GTO.svg [output.GTO]

Modes :
  --svg     Gerber -> SVG + JSON   (pour edition dans Inkscape)
  --gerber  SVG -> Gerber          (reconversion apres edition)

Extensions Gerber courantes (Eagle / KiCad) :
  .GTL  Top Copper          (cuivre face composants)
  .GBL  Bottom Copper       (cuivre face soudure)
  .GTO  Top Silkscreen      (serigraphie face composants)
  .GBO  Bottom Silkscreen   (serigraphie face soudure)
  .GTS  Top Soldermask      (vernis face composants)
  .GBS  Bottom Soldermask   (vernis face soudure)
  .GTP  Top Paste           (pate a souder composants)
  .GBP  Bottom Paste        (pate a souder soudure)
  .GKO  Board Outline       (contour du PCB)
  .GML  Milling Layer       (contour Eagle, = GKO)
  .DRL  Drill / Excellon    (percages — format different)

Options (--svg) :
  --outline board.GKO   Contour PCB en calque verrouille

-------------

rename_gerbers.sh
renomme les fichiers Gerber EagleCad en version "universelle"
