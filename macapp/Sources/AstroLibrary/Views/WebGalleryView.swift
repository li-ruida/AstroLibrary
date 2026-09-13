import SwiftUI
import WebKit

struct WebGalleryView: NSViewRepresentable {
    let url: URL
    let reloadToken: Int

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .default()
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.navigationDelegate = context.coordinator
        view.setValue(false, forKey: "drawsBackground")
        context.coordinator.lastReloadToken = reloadToken
        view.load(URLRequest(url: desktopURL))
        return view
    }

    func updateNSView(_ view: WKWebView, context: Context) {
        if view.url?.host != desktopURL.host || view.url?.port != desktopURL.port {
            view.load(URLRequest(url: desktopURL))
        } else if context.coordinator.lastReloadToken != reloadToken {
            context.coordinator.lastReloadToken = reloadToken
            view.reload()
        }
    }

    private var desktopURL: URL {
        var components = URLComponents(url: url, resolvingAgainstBaseURL: false)!
        components.queryItems = [URLQueryItem(name: "desktop", value: "1")]
        return components.url!
    }

    final class Coordinator: NSObject, WKNavigationDelegate {
        var lastReloadToken = 0

        func webView(
            _ webView: WKWebView,
            decidePolicyFor navigationAction: WKNavigationAction,
            decisionHandler: @escaping (WKNavigationActionPolicy) -> Void
        ) {
            guard let url = navigationAction.request.url else {
                decisionHandler(.cancel)
                return
            }
            if url.host == "127.0.0.1" || url.host == "localhost" {
                decisionHandler(.allow)
            } else {
                NSWorkspace.shared.open(url)
                decisionHandler(.cancel)
            }
        }
    }
}
