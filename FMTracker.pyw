"""Double-click to start FM Tracker (needs Python 3.8+ from python.org)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fmtracker.app import main  # noqa: E402

if __name__ == "__main__":
    main()
