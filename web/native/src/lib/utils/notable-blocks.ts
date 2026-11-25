export interface NotableBlock {
  height: number;
  label: string;
}

const genesis: NotableBlock = { height: 0, label: 'Genesis' };

// Historical blocks of the bitcoin main chain (activation heights as in the
// consensus rules).
const mainnet: NotableBlock[] = [
  genesis,
  { height: 170, label: 'First transfer, Satoshi to Hal Finney' },
  { height: 32256, label: 'First difficulty change' },
  { height: 57043, label: 'Pizza transaction (10,000 BTC)' },
  { height: 74638, label: 'Value overflow bug (bad block replaced)' },
  { height: 91842, label: 'Duplicate coinbase of 91,812 (BIP30)' },
  { height: 91880, label: 'Duplicate coinbase of 91,722 (BIP30)' },
  { height: 210000, label: 'First halving' },
  { height: 227931, label: 'BIP34 activation (height in coinbase)' },
  { height: 363725, label: 'BIP66 activation (strict DER)' },
  { height: 388381, label: 'BIP65 activation (CHECKLOCKTIMEVERIFY)' },
  { height: 419328, label: 'CSV activation (BIP68, 112, 113)' },
  { height: 420000, label: 'Second halving' },
  { height: 481824, label: 'SegWit activation' },
  { height: 630000, label: 'Third halving' },
  { height: 709632, label: 'Taproot activation' },
  { height: 774628, label: 'Largest block (3.96 MB)' },
  { height: 840000, label: 'Fourth halving' },
];

/** The notable blocks of a network at or below the top, genesis for any. */
export function notableBlocks(network: string | undefined, top?: number): NotableBlock[] {
  const blocks = network === 'mainnet' ? mainnet : [genesis];
  return top === undefined ? blocks : blocks.filter((block) => block.height <= top);
}
