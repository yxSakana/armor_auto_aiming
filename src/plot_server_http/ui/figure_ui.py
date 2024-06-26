# -*- coding: utf-8 -*-
"""  
  @projectName socket
  @file figure_ui.py
  @brief 
 
  @author yx 
  @date 2023-11-09 18:58
"""

import threading
from typing import Dict

from loguru import logger
import matplotlib.pyplot as plt
from matplotlib.backends.backend_qtagg import FigureCanvas
from matplotlib.backends.backend_qtagg import \
    NavigationToolbar2QT as NavigationToolbar
from matplotlib.backends.qt_compat import QtWidgets
from matplotlib.gridspec import GridSpec
from PyQt5.Qt import pyqtSignal
from PyQt5.Qt import QObject

from custom_axes.realtime_factory import RealtimeFactory


class FigureCanvasUi(QtWidgets.QMainWindow):
    def __init__(self, figure_canvas: FigureCanvas, _title: str = "View", parent: QObject = None):
        super().__init__(parent)
        self.setWindowTitle(_title)
        self._main_widget = QtWidgets.QWidget()
        self.setCentralWidget(self._main_widget)
        layout = QtWidgets.QVBoxLayout(self._main_widget)
        layout.addWidget(NavigationToolbar(figure_canvas, self))
        layout.addWidget(figure_canvas)
        self.show()


class FigureCanvasUiFactory(QObject):
    __ui_factory: Dict = {}
    __factory_lock: threading.Lock = threading.Lock()
    create_ui_sign = pyqtSignal(str, dict)

    def __init__(self):
        super().__init__(None)
        self.create_ui_sign.connect(self.create_ui)

    @staticmethod
    def create_ui(object_name: str, figure_pool: Dict):
        rows = int(figure_pool[object_name]["rows"])
        cols = int(figure_pool[object_name]["cols"])
        multiple_axes = figure_pool[object_name]["multiple_axes"]
        figure_canvas = FigureCanvas(plt.Figure(figsize=(5, 3)))
        grid_spec: GridSpec = GridSpec(rows, cols)
        for row in range(rows):
            for col in range(cols):
                current = multiple_axes[f"{row}{col}"]
                current["axes"] = RealtimeFactory.create_axes(figure_canvas, grid_spec, row, col, current)
        with FigureCanvasUiFactory.__factory_lock:
            ui = FigureCanvasUi(figure_canvas, object_name)
            FigureCanvasUiFactory.__ui_factory[object_name] = ui

        timer = figure_canvas.new_timer(80)
        timer.add_callback(figure_canvas.draw)
        timer.start()
        figure_pool[object_name]["timer"] = timer
        figure_pool[object_name]["figure_canvas"] = figure_canvas
        figure_pool[object_name]["grid_spec"] = grid_spec
