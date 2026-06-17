#!/bin/bash
# rename_gerbers.sh — Renomme les fichiers Gerber Eagle vers des extensions standard
# Usage: ./rename_gerbers.sh <dossier_gerbers>

DIR="${1:-.}"

if [ ! -d "$DIR" ]; then
    echo "Erreur: '$DIR' n'est pas un dossier."
    exit 1
fi

rename_ext() {
    local old_ext="$1"
    local new_ext="$2"
    local desc="$3"
    for f in "$DIR"/*."$old_ext" "$DIR"/*."$(echo "$old_ext" | tr '[:upper:]' '[:lower:]')"; do
        [ -f "$f" ] || continue
        base="${f%.*}"
        dest="${base}.${new_ext}"
        if [ "$f" != "$dest" ]; then
            echo "  $desc: $(basename "$f") -> $(basename "$dest")"
            mv "$f" "$dest"
        fi
    done
}

echo "=== Renommage Gerbers Eagle -> Standard ==="
echo "Dossier: $DIR"
echo ""

# Couche outline (le principal problème)
rename_ext "GML" "GKO" "Board Outline"
rename_ext "gml" "GKO" "Board Outline"

# Top
rename_ext "GTL" "GTL" "Top Copper"
rename_ext "GTS" "GTS" "Top Soldermask"
rename_ext "GTO" "GTO" "Top Silkscreen"
rename_ext "GTP" "GTP" "Top Paste"

# Bottom
rename_ext "GBL" "GBL" "Bottom Copper"
rename_ext "GBS" "GBS" "Bottom Soldermask"
rename_ext "GBO" "GBO" "Bottom Silkscreen"
rename_ext "GBP" "GBP" "Bottom Paste"

# Drills
rename_ext "TXT" "DRL" "Drill"
rename_ext "txt" "DRL" "Drill"
rename_ext "XLN" "DRL" "Drill"
rename_ext "xln" "DRL" "Drill"

# Fichiers info Eagle (inutiles pour la fabrication)
for ext in dri gpi DRI GPI; do
    for f in "$DIR"/*."$ext"; do
        [ -f "$f" ] || continue
        echo "  Info (ignoré): $(basename "$f") — peut être supprimé"
    done
done

echo ""
echo "=== Terminé ==="
