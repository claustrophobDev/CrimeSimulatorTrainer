# UI-макет для CrimeTrainer.
# Основной трейнер написан на C++, здесь только внешний вид.

import ctypes
import tkinter as tk
from ctypes import wintypes


GAME_PROCESS = "Crime Simulator.exe"

TH32CS_SNAPPROCESS = 0x00000002
MAX_PATH = 260


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * MAX_PATH),
    ]


def find_game():
    kernel32 = ctypes.windll.kernel32

    snapshot = kernel32.CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    )

    if snapshot == ctypes.c_void_p(-1).value:
        return 0

    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)

    pid = 0

    if kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
        while True:
            if entry.szExeFile.lower() == GAME_PROCESS.lower():
                pid = entry.th32ProcessID
                break

            if not kernel32.Process32NextW(
                snapshot,
                ctypes.byref(entry)
            ):
                break

    kernel32.CloseHandle(snapshot)

    return pid


class Switch(tk.Canvas):
    def __init__(self, parent, command=None):
        super().__init__(
            parent,
            width=38,
            height=22,
            bg=parent["bg"],
            highlightthickness=0,
            cursor="hand2"
        )

        self.enabled = False
        self.command = command

        self.bind("<Button-1>", self.click)

        self.draw()

    def click(self, _event=None):
        self.enabled = not self.enabled
        self.draw()

        if self.command:
            self.command(self.enabled)

    def draw(self):
        self.delete("all")

        if self.enabled:
            background = "#EDEDED"
            circle = "#111111"
            circle_x = 27
        else:
            background = "#303030"
            circle = "#999999"
            circle_x = 11

        self.create_rectangle(
            11,
            3,
            27,
            19,
            fill=background,
            outline=background
        )

        self.create_oval(
            3,
            3,
            19,
            19,
            fill=background,
            outline=background
        )

        self.create_oval(
            19,
            3,
            35,
            19,
            fill=background,
            outline=background
        )

        self.create_oval(
            circle_x - 7,
            4,
            circle_x + 7,
            18,
            fill=circle,
            outline=circle
        )


class FeatureRow(tk.Frame):
    def __init__(self, parent, name):
        super().__init__(
            parent,
            bg="#151515",
            height=46
        )

        self.pack_propagate(False)

        self.label = tk.Label(
            self,
            text=name,
            bg="#151515",
            fg="#E7E7E7",
            font=("Segoe UI", 10),
            anchor="w"
        )

        self.label.pack(
            side="left",
            padx=(14, 0)
        )

        self.switch = Switch(
            self,
            command=self.changed
        )

        self.switch.pack(
            side="right",
            padx=12
        )

    def changed(self, enabled):
        print(
            f"{self.label['text']}: "
            f"{'ON' if enabled else 'OFF'}"
        )


class CrimeTrainerUI:
    def __init__(self):
        self.root = tk.Tk()

        self.root.title("CrimeTrainer")
        self.root.geometry("440x480")
        self.root.resizable(False, False)

        self.root.configure(
            bg="#0C0C0C"
        )

        self.root.attributes(
            "-topmost",
            True
        )

        self.pid = 0

        self.build_ui()
        self.check_game()

    def build_ui(self):
        header = tk.Frame(
            self.root,
            bg="#0C0C0C",
            height=70
        )

        header.pack(
            fill="x",
            padx=18
        )

        header.pack_propagate(False)

        title = tk.Label(
            header,
            text="CrimeTrainer",
            bg="#0C0C0C",
            fg="#F2F2F2",
            font=("Segoe UI Semibold", 15)
        )

        title.pack(
            side="left"
        )

        self.status = tk.Label(
            header,
            text="Игра не найдена",
            bg="#0C0C0C",
            fg="#777777",
            font=("Segoe UI", 9)
        )

        self.status.pack(
            side="right"
        )

        separator = tk.Frame(
            self.root,
            bg="#252525",
            height=1
        )

        separator.pack(
            fill="x",
            padx=18
        )

        content = tk.Frame(
            self.root,
            bg="#0C0C0C"
        )

        content.pack(
            fill="both",
            expand=True,
            padx=18,
            pady=(14, 0)
        )

        features = [
            "Остановка времени",
            "Игнор полиции",
            "Игнор гражданских",
            "Игнор камер",
            "Игнор датчиков",
            "Бесконечные патроны",
            "Неразрушаемые предметы",
        ]

        for index, feature in enumerate(features):
            row = FeatureRow(
                content,
                feature
            )

            row.pack(
                fill="x",
                pady=(0, 6)
            )

        footer = tk.Frame(
            self.root,
            bg="#0C0C0C",
            height=48
        )

        footer.pack(
            fill="x",
            padx=18
        )

        footer.pack_propagate(False)

        hint = tk.Label(
            footer,
            text="TAB — скрыть / показать",
            bg="#0C0C0C",
            fg="#606060",
            font=("Segoe UI", 8)
        )

        hint.pack(
            side="left"
        )

        version = tk.Label(
            footer,
            text="v0.3.1",
            bg="#0C0C0C",
            fg="#444444",
            font=("Segoe UI", 8)
        )

        version.pack(
            side="right"
        )

    def check_game(self):
        pid = find_game()

        if pid:
            if pid != self.pid:
                self.pid = pid

            self.status.configure(
                text=f"Подключено · {pid}",
                fg="#8BD49C"
            )

        else:
            self.pid = 0

            self.status.configure(
                text="Игра не найдена",
                fg="#777777"
            )

        self.root.after(
            1000,
            self.check_game
        )

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    CrimeTrainerUI().run()