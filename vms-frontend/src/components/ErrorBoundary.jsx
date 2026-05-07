import React from 'react';

class ErrorBoundary extends React.Component {
  constructor(props) {
    super(props);
    this.state = { hasError: false, error: null, errorInfo: null };
  }

  static getDerivedStateFromError(error) {
    return { hasError: true };
  }

  componentDidCatch(error, errorInfo) {
    console.error("ErrorBoundary caught an error", error, errorInfo);
    this.setState({ error, errorInfo });
  }

  resetError = () => {
    this.setState({ hasError: false, error: null, errorInfo: null });
  };

  render() {
    if (this.state.hasError) {
      return (
        <div style={{ padding: '20px', background: '#222', color: '#ff5555', height: '100vh', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
          <h2>Something went wrong in the VMS UI.</h2>
          <details style={{ whiteSpace: 'pre-wrap', background: '#111', padding: '10px', borderRadius: '4px', maxWidth: '80%', overflow: 'auto' }}>
            <summary>Error Details</summary>
            {this.state.error && this.state.error.toString()}
            <br />
            {this.state.errorInfo && this.state.errorInfo.componentStack}
          </details>
          <div style={{ marginTop: '20px', display: 'flex', gap: '10px' }}>
            <button style={{ padding: '10px 20px', background: '#444', color: '#fff', border: 'none', borderRadius: '4px', cursor: 'pointer' }} onClick={this.resetError}>
              Try Again
            </button>
            <button style={{ padding: '10px 20px', background: '#666', color: '#fff', border: 'none', borderRadius: '4px', cursor: 'pointer' }} onClick={() => window.location.reload()}>
              Reload Application
            </button>
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}

export default ErrorBoundary;
