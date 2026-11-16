import React from 'react';
import ReactDOM from 'react-dom/client';
import { App } from '@/app/app';
import { AppProviders } from '@/app/providers';
import './index.css';
import { initSkin } from '../../shared/skins';

initSkin();

const rootElement = document.querySelector('#root');
if (!rootElement) {
  throw new Error('Root element not found');
}

ReactDOM.createRoot(rootElement).render(
  <React.StrictMode>
    <AppProviders>
      <App />
    </AppProviders>
  </React.StrictMode>
);
