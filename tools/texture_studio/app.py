import fnmatch
import os
import sys
import threading
import traceback

from PIL.ImageQt import ImageQt
from PySide6.QtCore import QAbstractTableModel, QModelIndex, QObject, QSettings, Qt, QThread, Signal
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import (QAbstractItemView, QApplication, QCheckBox, QComboBox, QFileDialog,
                               QFormLayout, QGroupBox, QHBoxLayout, QHeaderView, QLabel,
                               QLineEdit, QListWidget, QMainWindow, QMessageBox, QPlainTextEdit,
                               QProgressBar, QPushButton, QSplitter, QTableView, QTabWidget,
                               QVBoxLayout, QWidget)

from texture_studio import textures

FORMAT_FILTERS = {
    'All formats': None,
    'Colour (DXT1)': {'DXT1', 'DXT1c', 'DXT1a'},
    'Colour + alpha (DXT5)': {'DXT5'},
    'Normal maps': {'DXT5_NM'},
    'Uncompressed': {'RGBA8', 'RGBX8', 'L8', 'A8'},
}
SIZE_FILTERS = {'Any size': 0, '256+': 256, '512+': 512, '1024+': 1024, '2048+': 2048}
COLUMNS = ['Name', 'Group', 'Size', 'Format', 'Hash', 'Exported']


class Job(QObject):
    progress = Signal(int, int, str)
    finished = Signal(object)
    failed = Signal(str)

    def __init__(self, work):
        super().__init__()
        self.work = work
        self.cancelled = threading.Event()

    def report(self, done, total, text):
        self.progress.emit(done, total, text)
        return not self.cancelled.is_set()

    def run(self):
        try:
            self.finished.emit(self.work(self.report))
        except Exception:
            self.failed.emit(traceback.format_exc())


class TextureModel(QAbstractTableModel):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.exported = set()

    def set_rows(self, rows, exported):
        self.beginResetModel()
        self.rows = rows
        self.exported = exported
        self.endResetModel()

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.rows)

    def columnCount(self, parent=QModelIndex()):
        return len(COLUMNS)

    def headerData(self, section, orientation, role=Qt.DisplayRole):
        if role == Qt.DisplayRole and orientation == Qt.Horizontal:
            return COLUMNS[section]
        return None

    def data(self, index, role=Qt.DisplayRole):
        if role != Qt.DisplayRole:
            return None
        r = self.rows[index.row()]
        return [r['name'], r['group'], f"{r['width']}x{r['height']}", r['format'], r['hash'],
                'yes' if r['hash'] in self.exported else ''][index.column()]

    def sort(self, column, order=Qt.AscendingOrder):
        keys = [lambda r: r['name'].lower(), lambda r: r['group'].lower(),
                lambda r: r['width'] * r['height'], lambda r: r['format'], lambda r: r['hash'],
                lambda r: r['hash'] in self.exported]
        self.beginResetModel()
        self.rows.sort(key=keys[column], reverse=order == Qt.DescendingOrder)
        self.endResetModel()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Dante's Inferno Texture Studio")
        self.resize(1300, 820)
        self.settings = QSettings('DantesInferno', 'TextureStudio')
        self.index = {}
        self.manifest = {}
        self.thread = None
        self.job = None

        self.game_edit = QLineEdit(self.settings.value('game', self.default_game()))
        self.project_edit = QLineEdit(self.settings.value('project', ''))
        paths = QFormLayout()
        paths.addRow('Game folder', self.path_row(self.game_edit, 'Select the game folder'))
        paths.addRow('Project folder', self.path_row(self.project_edit, 'Select a project folder'))

        tabs = QTabWidget()
        tabs.addTab(self.textures_tab(), 'Textures')
        tabs.addTab(self.build_tab(), 'Build mod')

        self.progress_bar = QProgressBar()
        self.status = QLabel('Ready')
        self.cancel_button = QPushButton('Cancel')
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.cancel_job)
        bottom = QHBoxLayout()
        bottom.addWidget(self.status, 1)
        bottom.addWidget(self.progress_bar, 1)
        bottom.addWidget(self.cancel_button)

        root = QVBoxLayout()
        root.addLayout(paths)
        root.addWidget(tabs, 1)
        root.addLayout(bottom)
        central = QWidget()
        central.setLayout(root)
        self.setCentralWidget(central)

        self.project_edit.editingFinished.connect(self.load_project)
        self.game_edit.editingFinished.connect(self.save_settings)
        self.load_project()

    @staticmethod
    def default_game():
        for candidate in ('game', os.path.join('..', 'game')):
            if os.path.exists(os.path.join(candidate, 'bigfile0.viv')):
                return os.path.abspath(candidate)
        return ''

    def path_row(self, edit, title):
        button = QPushButton('Browse...')

        def browse():
            path = QFileDialog.getExistingDirectory(self, title, edit.text())
            if path:
                edit.setText(path)
                edit.editingFinished.emit()

        button.clicked.connect(browse)
        row = QHBoxLayout()
        row.addWidget(edit, 1)
        row.addWidget(button)
        widget = QWidget()
        widget.setLayout(row)
        row.setContentsMargins(0, 0, 0, 0)
        return widget

    def textures_tab(self):
        self.scan_button = QPushButton('Scan game')
        self.scan_button.clicked.connect(self.scan_game)
        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText('Filter by name, group or hash (wildcards allowed, e.g. hero_*_c)')
        self.search_edit.textChanged.connect(self.apply_filter)
        self.format_combo = QComboBox()
        self.format_combo.addItems(FORMAT_FILTERS)
        self.format_combo.currentTextChanged.connect(self.apply_filter)
        self.size_combo = QComboBox()
        self.size_combo.addItems(SIZE_FILTERS)
        self.size_combo.currentTextChanged.connect(self.apply_filter)
        self.exported_check = QCheckBox('Exported only')
        self.exported_check.toggled.connect(self.apply_filter)
        filters = QHBoxLayout()
        filters.addWidget(self.scan_button)
        filters.addWidget(self.search_edit, 1)
        filters.addWidget(self.format_combo)
        filters.addWidget(self.size_combo)
        filters.addWidget(self.exported_check)

        self.model = TextureModel()
        self.table = QTableView()
        self.table.setModel(self.model)
        self.table.setSortingEnabled(True)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.table.selectionModel().currentRowChanged.connect(self.show_preview)

        self.preview = QLabel('No texture selected')
        self.preview.setAlignment(Qt.AlignCenter)
        self.preview.setMinimumSize(380, 380)
        self.preview.setStyleSheet('background: #202020; color: #aaa;')
        self.details = QLabel()
        self.details.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.details.setWordWrap(True)
        export_selected = QPushButton('Export selected')
        export_selected.clicked.connect(lambda: self.export(self.selected_rows()))
        export_shown = QPushButton('Export all shown')
        export_shown.clicked.connect(lambda: self.export(list(self.model.rows)))
        open_png = QPushButton('Open PNG')
        open_png.clicked.connect(self.open_png)
        open_project = QPushButton('Open project folder')
        open_project.clicked.connect(lambda: self.open_path(self.project_edit.text()))
        side = QVBoxLayout()
        side.addWidget(self.preview, 1)
        side.addWidget(self.details)
        for button in (export_selected, export_shown, open_png, open_project):
            side.addWidget(button)
        side_widget = QWidget()
        side_widget.setLayout(side)

        splitter = QSplitter()
        splitter.addWidget(self.table)
        splitter.addWidget(side_widget)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 1)
        self.count_label = QLabel()
        layout = QVBoxLayout()
        layout.addLayout(filters)
        layout.addWidget(splitter, 1)
        layout.addWidget(self.count_label)
        widget = QWidget()
        widget.setLayout(layout)
        return widget

    def build_tab(self):
        self.mod_name_edit = QLineEdit(self.settings.value('mod_name', 'MyTextureMod'))
        self.mod_author_edit = QLineEdit(self.settings.value('mod_author', ''))
        self.mod_version_edit = QLineEdit(self.settings.value('mod_version', '1.0'))
        self.mods_dir_edit = QLineEdit(self.settings.value('mods_dir', ''))
        form = QFormLayout()
        form.addRow('Mod name', self.mod_name_edit)
        form.addRow('Author', self.mod_author_edit)
        form.addRow('Version', self.mod_version_edit)
        form.addRow('Mods folder', self.path_row(self.mods_dir_edit, 'Select the mods folder'))
        hint = QLabel('The mods folder sits next to dantes_inferno.exe. Edited PNGs from the '
                      'project folder are written there as <mods>/<mod name>/textures/*.dds; '
                      'any resolution works as long as the aspect ratio is kept.')
        hint.setWordWrap(True)
        form.addRow(hint)
        box = QGroupBox('Mod')
        box.setLayout(form)

        self.changed_list = QListWidget()
        refresh = QPushButton('Find edited textures')
        refresh.clicked.connect(self.refresh_changed)
        build = QPushButton('Build mod')
        build.clicked.connect(self.build_mod)
        buttons = QHBoxLayout()
        buttons.addWidget(refresh)
        buttons.addWidget(build)
        buttons.addStretch(1)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)

        layout = QVBoxLayout()
        layout.addWidget(box)
        layout.addLayout(buttons)
        layout.addWidget(QLabel('Edited textures'))
        layout.addWidget(self.changed_list, 2)
        layout.addWidget(QLabel('Log'))
        layout.addWidget(self.log, 1)
        widget = QWidget()
        widget.setLayout(layout)
        return widget

    def save_settings(self):
        self.settings.setValue('game', self.game_edit.text())
        self.settings.setValue('project', self.project_edit.text())
        self.settings.setValue('mod_name', self.mod_name_edit.text())
        self.settings.setValue('mod_author', self.mod_author_edit.text())
        self.settings.setValue('mod_version', self.mod_version_edit.text())
        self.settings.setValue('mods_dir', self.mods_dir_edit.text())

    def closeEvent(self, event):
        self.save_settings()
        if self.job:
            self.job.cancelled.set()
        super().closeEvent(event)

    def require(self, game=False, project=True):
        if game and not os.path.exists(os.path.join(self.game_edit.text(), 'bigfile0.viv')):
            QMessageBox.warning(self, 'Game folder', 'Select the game folder (with bigfile0.viv).')
            return False
        if project and not self.project_edit.text():
            QMessageBox.warning(self, 'Project folder', 'Select a project folder first.')
            return False
        return True

    def load_project(self):
        self.save_settings()
        project = self.project_edit.text()
        self.index = textures.load_index(project) if project else {}
        self.manifest = textures.load_manifest(project) if project else {}
        self.apply_filter()

    def apply_filter(self):
        pattern = self.search_edit.text().strip().lower()
        if pattern and not any(c in pattern for c in '*?['):
            pattern = f'*{pattern}*'
        formats = FORMAT_FILTERS[self.format_combo.currentText()]
        min_size = SIZE_FILTERS[self.size_combo.currentText()]
        exported = {e['hash'] for e in self.manifest.values()}
        rows = [r for r in self.index.values()
                if (not pattern or any(fnmatch.fnmatch(n.lower(), pattern)
                                       for n in [r['name'], r['group'], r['hash'],
                                                 *r.get('aliases', [])]))
                and (formats is None or r['format'] in formats)
                and max(r['width'], r['height']) >= min_size
                and (not self.exported_check.isChecked() or r['hash'] in exported)]
        rows.sort(key=lambda r: r['name'].lower())
        self.model.set_rows(rows, exported)
        self.count_label.setText(f'{len(rows)} of {len(self.index)} textures, '
                                 f'{len(exported)} exported')

    def selected_rows(self):
        return [self.model.rows[i.row()] for i in self.table.selectionModel().selectedRows()]

    def png_path(self, record):
        return os.path.join(self.project_edit.text(), textures.relative_png(record))

    def show_preview(self, current, _previous):
        if not current.isValid():
            return
        record = self.model.rows[current.row()]
        locations = ', '.join(f'{v} #{e}' for v, e, _ in record['locations'][:4])
        more = len(record['locations']) - 4
        self.details.setText(
            f"<b>{record['name']}</b><br>{record['group']}<br>"
            f"{record['width']}x{record['height']} {record['format']} "
            f"{' '.join(record['flags'])}<br>hash {record['hash']}<br>"
            f"{'also named ' + ', '.join(record['aliases']) + '<br>' if record.get('aliases') else ''}"
            f"in {locations}{f' and {more} more' if more > 0 else ''}")
        path = self.png_path(record)
        if os.path.exists(path):
            self.set_preview(QPixmap(path))
            return
        game = self.game_edit.text()
        self.preview.setText('Loading...')

        def load(report):
            info, tg4d = textures.read_texture(game, record)
            return record['hash'], textures.decode(info, tg4d)

        def done(result):
            selected = self.table.currentIndex()
            if selected.isValid() and self.model.rows[selected.row()]['hash'] == result[0]:
                self.set_preview(QPixmap.fromImage(ImageQt(result[1])))

        self.start_job(load, done, quiet=True)

    def set_preview(self, pixmap):
        self.preview.setPixmap(pixmap.scaled(self.preview.size(), Qt.KeepAspectRatio,
                                             Qt.SmoothTransformation))

    def open_png(self):
        rows = self.selected_rows()
        if rows and os.path.exists(self.png_path(rows[0])):
            self.open_path(self.png_path(rows[0]))
        elif rows:
            QMessageBox.information(self, 'Open PNG', 'Export the texture first.')

    @staticmethod
    def open_path(path):
        if path and os.path.exists(path):
            os.startfile(path)

    def scan_game(self):
        if not self.require(game=True):
            return
        game, project = self.game_edit.text(), self.project_edit.text()

        def scan(report):
            index = textures.scan_game(game, report)
            textures.save_json(os.path.join(project, textures.INDEX_FILE), index)
            return index

        def done(index):
            self.index = index
            self.apply_filter()
            self.status.setText(f'Scan finished: {len(index)} textures')

        self.start_job(scan, done)

    def export(self, rows):
        if not rows or not self.require(game=True):
            return
        if len(rows) > 500 and QMessageBox.question(
                self, 'Export', f'Export {len(rows)} textures?') != QMessageBox.Yes:
            return
        game, project = self.game_edit.text(), self.project_edit.text()

        def done(count):
            self.manifest = textures.load_manifest(project)
            self.apply_filter()
            self.status.setText(f'Exported {count} textures to {project}')

        self.start_job(lambda report: textures.export_textures(game, project, rows, report), done)

    def refresh_changed(self):
        if not self.require():
            return
        project = self.project_edit.text()

        def done(changed):
            self.changed_list.clear()
            for rel, entry in changed:
                self.changed_list.addItem(f"{rel}  ({entry['width']}x{entry['height']} original)")
            self.status.setText(f'{len(changed)} edited textures')

        self.start_job(lambda report: textures.changed_textures(project), done)

    def build_mod(self):
        if not self.require():
            return
        name = textures.safe_part(self.mod_name_edit.text().strip())
        mods_dir = self.mods_dir_edit.text()
        if not mods_dir and self.game_edit.text():
            install_dir = os.path.dirname(os.path.normpath(self.game_edit.text()))
            mods_dir = os.path.join(install_dir, 'mods')
            self.mods_dir_edit.setText(mods_dir)
        if not mods_dir:
            QMessageBox.warning(self, 'Mods folder', 'Select the mods folder first.')
            return
        self.save_settings()
        project = self.project_edit.text()
        mod_dir = os.path.join(mods_dir, name)
        author, version = self.mod_author_edit.text(), self.mod_version_edit.text()

        def done(result):
            count, warnings = result
            for warning in warnings:
                self.log.appendPlainText(f'Warning: {warning}')
            self.log.appendPlainText(f'Built {name}: {count} textures in {mod_dir}')
            self.status.setText(f'Built {name}')

        self.start_job(lambda report: textures.build_mod(project, mod_dir, name, author, version,
                                                         report), done)

    def start_job(self, work, done, quiet=False):
        if self.job:
            if quiet:
                return
            QMessageBox.information(self, 'Busy', 'Wait for the current task to finish.')
            return
        self.job = Job(work)
        self.thread = QThread()
        self.job.moveToThread(self.thread)
        self.thread.started.connect(self.job.run)
        self.job.progress.connect(self.on_progress)
        self.job.finished.connect(done)
        self.job.failed.connect(self.on_failed)
        self.job.finished.connect(self.end_job)
        self.job.failed.connect(self.end_job)
        self.cancel_button.setEnabled(not quiet)
        if not quiet:
            self.progress_bar.setRange(0, 0)
        self.thread.start()

    def end_job(self, *_):
        self.thread.quit()
        self.thread.wait()
        self.job = None
        self.thread = None
        self.cancel_button.setEnabled(False)
        self.progress_bar.setRange(0, 1)
        self.progress_bar.setValue(1)

    def cancel_job(self):
        if self.job:
            self.job.cancelled.set()
            self.status.setText('Cancelling...')

    def on_progress(self, done, total, text):
        self.progress_bar.setRange(0, max(total, 1))
        self.progress_bar.setValue(done)
        self.status.setText(f'{done}/{total}  {text}')

    def on_failed(self, error):
        self.log.appendPlainText(error)
        QMessageBox.critical(self, 'Error', error.strip().splitlines()[-1])


def run():
    app = QApplication(sys.argv)
    app.setApplicationName('Texture Studio')
    window = MainWindow()
    window.show()
    return app.exec()
