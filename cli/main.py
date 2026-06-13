try:
    from .gui import main
except ImportError:
    from cli.gui import main


if __name__ == "__main__":
    raise SystemExit(main())
