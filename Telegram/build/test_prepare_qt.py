import ast
import re
import unittest
from pathlib import Path


class QtRecipeTest(unittest.TestCase):
    def commands(self, options):
        source = Path(__file__).with_name('prepare') / 'prepare.py'
        tree = ast.parse(source.read_text())
        helpers = [node for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name in ('filterByPlatform', 'removeDir')]
        recipes = [node for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == 'stage'
            and isinstance(node.args[0], ast.BinOp)
            and ast.unparse(node.args[0]) == "'qt_' + qt"]
        context = dict(
            re=re, win=True, win32=False, win64=True, winarm=False,
            mac=False, qt='6.11.2', branch='v$QT', qtAngleCmake='qt.cmake',
            computeFileHash=lambda path: 'fixture', options=options)
        exec(compile(ast.Module(body=helpers, type_ignores=[]),
            str(source), 'exec'), context)
        recipe = eval(compile(ast.Expression(recipes[-1].args[1]),
            str(source), 'eval'), context)
        return context['filterByPlatform'](recipe)[0]

    def test_release_only(self):
        commands = self.commands(['qt6', 'qt-release-only'])
        self.assertIn('SET CONFIGURATIONS=-release', commands)
        self.assertNotIn('cmake --build . --config Debug', commands)
        self.assertNotIn('cmake --install . --config Debug', commands)
        self.assertNotIn('-force-debug-info', commands)
        self.assertIn('cmake --build .\ncmake --install .', commands)

    def test_default_keeps_both_configurations(self):
        commands = self.commands(['qt6'])
        self.assertIn('SET CONFIGURATIONS=-debug-and-release', commands)
        self.assertIn('cmake --build . --config Debug', commands)
        self.assertIn('cmake --install . --config Debug', commands)
        self.assertIn('-force-debug-info', commands)

    def test_debug_only(self):
        commands = self.commands(['qt6', 'skip-release'])
        self.assertIn('SET CONFIGURATIONS=-debug\n', commands)
        self.assertNotIn('SET CONFIGURATIONS=-debug-and-release', commands)
        self.assertIn('cmake --build . --config Debug', commands)


if __name__ == '__main__':
    unittest.main()
