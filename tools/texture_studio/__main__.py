import argparse
import fnmatch
import multiprocessing
import os
import sys

from texture_studio import textures


def report(done, total, text):
    print(f'\r{done}/{total} {text}'[:120].ljust(120), end='', flush=True)
    return True


def cli(argv):
    parser = argparse.ArgumentParser(prog='TextureStudio',
                                     description="Dante's Inferno texture modding tool")
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('scan', help='index every texture in the game')
    p.add_argument('game')
    p.add_argument('project')
    p = sub.add_parser('export', help='export indexed textures to PNG')
    p.add_argument('game')
    p.add_argument('project')
    p.add_argument('--name', action='append', default=[], help='name glob, repeatable')
    p = sub.add_parser('build', help='write edited PNGs as a mod')
    p.add_argument('project')
    p.add_argument('mod_dir')
    p.add_argument('--author', default='')
    p.add_argument('--version', default='1.0')
    args = parser.parse_args(argv)

    if args.command == 'scan':
        index = textures.scan_game(args.game, report)
        textures.save_json(os.path.join(args.project, textures.INDEX_FILE), index)
        print(f'\n{len(index)} textures indexed')
    elif args.command == 'export':
        index = textures.load_index(args.project)
        rows = [r for r in index.values()
                if not args.name or any(fnmatch.fnmatch(a, n) for n in args.name
                                         for a in [r['name'], *r.get('aliases', [])])]
        count = textures.export_textures(args.game, args.project, rows, report)
        print(f'\n{count} textures exported')
    else:
        name = os.path.basename(os.path.normpath(args.mod_dir))
        count, warnings = textures.build_mod(args.project, args.mod_dir, name, args.author,
                                             args.version, report)
        print()
        for warning in warnings:
            print(f'Warning: {warning}')
        print(f'{count} textures written to {args.mod_dir}')


def main():
    multiprocessing.freeze_support()
    if len(sys.argv) > 1:
        cli(sys.argv[1:])
    else:
        from texture_studio import app
        sys.exit(app.run())


if __name__ == '__main__':
    main()
