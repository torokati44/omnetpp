# In Graphviz 2.x, dot-generated SVGs are faulty (view box too small), this script fixes it.
# See https://github.com/omnetpp/omnetpp/issues/1048, https://gitlab.com/graphviz/graphviz/-/issues/1406

import sys
import re

if len(sys.argv) not in [2, 3]:
    print(f"Usage: python {sys.argv[0]} input.svg [viewbox-enhancing-factor]")
    sys.exit(1)

input_file = sys.argv[1]
factor = float(sys.argv[2]) if len(sys.argv) == 3 else 2.0

with open(input_file, "r") as file:
    svg_text = file.read()

if re.search(r"Generated by graphviz version 2\.\d+\.\d+", svg_text):
    # Enlarge viewBox
    svg_text = re.sub(r'viewBox="([^"]+)"', lambda match: 'viewBox="' + ' '.join(str(float(value) * factor) for value in match.group(1).split()) + '"', svg_text)

    # Save the modified SVG back to the same file
    with open(input_file, "w") as file:
        file.write(svg_text)
    print(f"ViewBox modified and saved to {input_file}")
else:
    print(f"Graphviz is NOT of version 2.x, ViewBox is probably OK, not touching it")
