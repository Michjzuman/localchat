import shutil

w, h = shutil.get_terminal_size()

def my_message(name, message):
    print("\n".join([
        " " * (w - len(line)) + line
        for line in [
        f"{name}   ",
        f"╭─{'─' * len(message)}─╮ ",
        f"│ {message} │❯",
        f"╰─{'─' * len(message)}─╯ ",
    ]]))

def others_message(name, message):
    print("\n".join([
        f"  {name}",
        f" ╭─{'─' * len(message)}─╮",
        f"❮│ {message} │",
        f" ╰─{'─' * len(message)}─╯",
    ]))

print()
print()
print()

my_message("micha", "Hello World!")
others_message("hans", "Blabla")
others_message("hans", "BliBlu")
others_message("peter", "Haha")

print()
print()

input("\n".join([
    f"╭{'─' * (w - 2)}╮",
    f"│{' ' * (w - 4)}⏎ │",
    f"╰{'─' * (w - 2)}╯\033[1F\033[2C"
]))