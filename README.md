# Kerything 🔍

> [!NOTE]
> This branch contains the rewrite of the Kerything project.

## Socket permissions

To allow the Kerything daemon to bind to the socket and allow the GUI process to communicate with it, create a `kerything` group and add your user to it.

```sh
sudo groupadd kerything
sudo usermod -aG kerything yourusername
```

You will need to restart your computer for the changes to take effect.

## 📜 License

This project is licensed under the **GPL-3.0-or-later** License - see the [LICENSE](LICENSE) file for details.
