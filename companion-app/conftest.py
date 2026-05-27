import os
import sys

# Allow `import loop_logic` from tests without packaging the app.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
